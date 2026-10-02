// ————————————————————————————————————————————————————————————————————————————
// JZJSCvModule.cpp —— __jz.cv.*（OpenCV 传统图像处理 → JS）
//
// 为什么是"句柄 + 表驱动"而不是"每个 API 一个绑定函数":
//   · 图像必须是**句柄**(id), 不能每步都把像素搬成 JSON/数组 —— 一次 480x360x3 就是 500KB;
//     句柄让"截屏 → 去噪 → 分割 → 形态学 → 找轮廓"这条链完全留在 C++ 里, JS 只传一个数字。
//   · 运算统一走 op(name, paramsJson) 一个口: 加一项能力 = ops 表里加 6 行, 不用碰 JS 侧绑定面
//     (也就不用给每个 API 单独想一套参数怎么摊平的规则)。
//   · 参数一律 JSON 对象, 图像参数写 id: {"src":3,"ksize":[5,5],"sigmaX":1.5}
//     形状与 OpenCV 自己的签名一一对应, 看 OpenCV 文档就知道该传什么。
//
// 命名: 码表(interpolation / borderType / morphOp / colormap / ...)用**字符串**, 大小写与
//   `CV_`/`COLOR_`/`MORPH_`/`INTER_` 之类前缀都能省, 例如 'bgr2gray' / 'COLOR_BGR2GRAY' / 'BGR2GRAY' 等价。
//
// 失败: op 返回 {ok:false, error:"..."}; 原子 API 用 0/空串/{error}. 绝不返回"看着成功的空图"。
// 缺 OpenCV 的构建(JZ_CU_OPENCV 未定义): 一律报 "未编入", 见文件末尾的 #else 分支。
// ————————————————————————————————————————————————————————————————————————————
#include "JZJSCvModule.h"
#include "JZJSBind.h"                        // jsbind::createSelfFunction
#include <json/json.h>
#include <map>
#include <vector>
#include <string>
#include <functional>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cstring>

// 构造/析构要在 .cpp 里(PIMPL: unique_ptr<Impl> 需要完整类型)
JZJSCvModule::JZJSCvModule() = default;
JZJSCvModule::~JZJSCvModule() = default;

#if JZ_CU_OPENCV
// ============================================================================
// 编入 OpenCV 的版本
// ============================================================================
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
// 传统算法补充(2026-10-03)用到的另外两个模块: 标定几何(RANSAC/LMEDS/findHomography)与
// 光流(calcOpticalFlowFarneback)。链接上都是同一个 opencv_world 单库, 只是头文件要显式带进来。
// ⚠ photo 模块在本机的 OpenCV 构建里是 **OFF**(BUILD_opencv_photo=OFF, 见 E:/libs/opencv-4.10.0),
//   所以 fastNlMeansDenoising / inpaint 这类"修复"算法没有 —— 不是没实现, 是这个库没编进去。
#include <opencv2/calib3d.hpp>
#include <opencv2/video/tracking.hpp>

namespace {

using cv::Mat;
using cv::Size;
using cv::Point;
using cv::Point2f;                    // getPerspectiveTransform 用
using cv::Rect;
using cv::Scalar;

// —— 统一返回信封 ——
Json::Value jOk() { Json::Value v; v["ok"] = true; return v; }
Json::Value jOkMat(int id) { Json::Value v = jOk(); v["mat"] = id; return v; }
Json::Value jOkMats(const std::vector<int>& ids) {
    Json::Value v = jOk(), a(Json::arrayValue);
    for (int i : ids) a.append(i);
    v["mats"] = a; return v;
}
Json::Value jOkVal(const Json::Value& val) { Json::Value v = jOk(); v["value"] = val; return v; }
Json::Value jFail(const std::string& msg) { Json::Value v; v["ok"] = false; v["error"] = msg; return v; }

// —— 小工具 ——
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
// 只留字母数字并转小写, 再剥掉一个**最长匹配**的常量前缀 —— 于是 'COLOR_BGR2GRAY' /
// 'color.bgr2gray' / 'bgr2gray' 得到同一个键。表键都写短形式(如 "bgr2gray" / "minmax" / "open")。
std::string normKey(const std::string& s) {
    std::string t;
    for (char c : s) if (std::isalnum((unsigned char)c)) t.push_back((char)std::tolower((unsigned char)c));
    static const char* kPre[] = {                  // ⚠ 按长度降序(colormap 必须在 color 之前)
        "colormap", "threshold", "interpolation", "adaptive", "hamming",
        "border", "morph", "thresh", "inter", "color", "chain", "retr", "norm", "font", "line", "cmp", "tm", "cv",
    };
    for (const char* pre : kPre) {
        const size_t n = strlen(pre);
        if (t.size() > n && t.compare(0, n, pre) == 0) { t = t.substr(n); break; }
    }
    return t;
}

// 码表参数也收**OpenCV 的整型枚举**本身(如 cv::COLOR_BGR2GRAY = 6 / THRESH_BINARY|OTSU = 8):
// TS 侧的 OpenCV.js 风格常量表(cv.COLOR_* / cv.THRESH_*)就是数字, 直接透传比再查一次名字表更省。
bool numericKey(const std::string& s, int& out) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    out = atoi(s.c_str());
    return true;
}
// 码表参数取值: 字符串(名称/前缀随意, 见 normKey) 或 数字(整型枚举) 都收
std::string strNumOf(const Json::Value& p, const char* k, const char* def) {
    if (!p.isMember(k)) return std::string(def);
    if (p[k].isString()) return p[k].asString();
    if (p[k].isNumeric()) return std::to_string(p[k].asInt());
    return std::string(def);
}

// —— base64(自带, 不引第三方) ——
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string b64Encode(const unsigned char* d, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = d[i] << 16;
        if (i + 1 < n) v |= d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? kB64[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? kB64[v & 63] : '=');
    }
    return out;
}
int b64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
// 收 base64 / dataURL(自动剥掉 'data:image/png;base64,' 前缀), 忽略空白
bool b64Decode(const std::string& in, std::vector<unsigned char>& out) {
    std::string s = in;
    const size_t comma = s.find("base64,");
    if (comma != std::string::npos) s = s.substr(comma + 7);
    int buf = 0, bits = 0;
    for (char c : s) {
        if (std::isspace((unsigned char)c) || c == '"') continue;
        if (c == '=') break;
        const int v = b64Val(c);
        if (v < 0) return false;
        buf = (buf << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((unsigned char)((buf >> bits) & 0xFF)); }
    }
    return !out.empty();
}

// 单通道直通(不 clone); 多通道转灰度 —— 阈值/边缘/轮廓这些"只吃灰度"的 op 都走它
Mat toGray(const Mat& src) {
    if (src.channels() == 1) return src;
    Mat g;
    cv::cvtColor(src, g, src.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
    return g;
}

// —— JSON 取参 ——
double numOf(const Json::Value& p, const char* k, double def) {
    return (p.isMember(k) && p[k].isNumeric()) ? p[k].asDouble() : def;
}
int intOf(const Json::Value& p, const char* k, int def) {
    return (p.isMember(k) && p[k].isNumeric()) ? p[k].asInt() : def;
}
bool boolOf(const Json::Value& p, const char* k, bool def) {
    if (!p.isMember(k)) return def;
    if (p[k].isBool()) return p[k].asBool();
    if (p[k].isNumeric()) return p[k].asInt() != 0;
    return def;
}
std::string strOf(const Json::Value& p, const char* k, const char* def) {
    return (p.isMember(k) && p[k].isString()) ? p[k].asString() : std::string(def);
}
int matIdOf(const Json::Value& p, const char* k) {
    if (!p.isMember(k) || !p[k].isNumeric())
        throw std::runtime_error(std::string("缺少图像参数 '") + k + "' (要传图像句柄 id)");
    return p[k].asInt();
}
// [w,h] / 数字(正方形) / {w,h}
Size sizeOf(const Json::Value& p, const char* k, Size def) {
    if (!p.isMember(k)) return def;
    const Json::Value& v = p[k];
    if (v.isArray() && v.size() >= 2) return Size(v[0u].asInt(), v[1].asInt());
    if (v.isNumeric()) return Size(v.asInt(), v.asInt());
    if (v.isObject()) return Size(intOf(v, "w", def.width), intOf(v, "h", def.height));
    return def;
}
// [x,y] / {x,y}
Point pointOf(const Json::Value& v, Point def = Point(0, 0)) {
    if (v.isArray() && v.size() >= 2) return Point(v[0u].asInt(), v[1].asInt());
    if (v.isObject() && v.isMember("x") && v.isMember("y")) return Point(v["x"].asInt(), v["y"].asInt());
    return def;
}
Point pointOf(const Json::Value& p, const char* k, Point def = Point(0, 0)) {
    return p.isMember(k) ? pointOf(p[k], def) : def;
}
// {x,y,w,h} / [x,y,w,h]
Rect rectOf(const Json::Value& v, Rect def = Rect()) {
    if (v.isObject() && v.isMember("w")) return Rect(v["x"].asInt(), v["y"].asInt(), v["w"].asInt(), v["h"].asInt());
    if (v.isArray() && v.size() >= 4) return Rect(v[0u].asInt(), v[1].asInt(), v[2].asInt(), v[3].asInt());
    return def;
}
// 数字 / [v] / [b,g,r] / [b,g,r,a] / '#RRGGBB'(按 RGB 读, 内部换 BGR)
Scalar scalarOf(const Json::Value& p, const char* k, Scalar def = Scalar(0, 0, 0, 0)) {
    if (!p.isMember(k)) return def;
    const Json::Value& v = p[k];
    if (v.isNumeric()) return Scalar(v.asDouble(), v.asDouble(), v.asDouble(), v.asDouble());
    if (v.isArray() && v.size() > 0) {
        double a[4] = { 0, 0, 0, 0 };
        for (Json::Value::ArrayIndex i = 0; i < v.size() && i < 4; ++i) a[i] = v[i].asDouble();
        if (v.size() == 1) return Scalar(a[0], a[0], a[0], a[0]);
        return Scalar(a[0], a[1], a[2], v.size() >= 4 ? a[3] : 0.0);
    }
    if (v.isString()) {
        std::string s = v.asString();
        if (!s.empty() && s[0] == '#') s = s.substr(1);
        if (s.size() == 6) {
            const long n = strtol(s.c_str(), nullptr, 16);
            return Scalar(n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, 0);
        }
    }
    return def;
}
// 核: 数字(n x n 全 1) / [[..],[..]] / {data:[[..]]} / {shape:'rect'|'ellipse'|'cross',ksize:[w,h]}
Mat kernelOf(const Json::Value& p, const char* k) {
    if (!p.isMember(k)) return Mat();
    const Json::Value& v = p[k];
    if (v.isNumeric()) { const int n = v.asInt(); return Mat::ones(n, n, CV_32F); }
    if (v.isObject() && v.isMember("shape")) {
        int shape = cv::MORPH_RECT;
        const std::string s = lower(strOf(v, "shape", "rect"));
        if (s.find("ellip") != std::string::npos) shape = cv::MORPH_ELLIPSE;
        else if (s.find("cross") != std::string::npos) shape = cv::MORPH_CROSS;
        return cv::getStructuringElement(shape, sizeOf(v, "ksize", Size(3, 3)));
    }
    const Json::Value* d = &v;
    if (v.isObject() && v.isMember("data")) d = &v["data"];
    if (d->isArray() && d->size() > 0 && (*d)[0u].isArray()) {
        const int rows = (int)d->size(), cols = (int)(*d)[0u].size();
        Mat m(rows, cols, CV_32F);
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < cols; ++x)
                m.at<float>(y, x) = (float)(*d)[(Json::Value::ArrayIndex)y][(Json::Value::ArrayIndex)x].asDouble();
        return m;
    }
    return Mat();
}

// —— 类型名 <-> CV 类型 ——
struct TypeName { const char* name; int type; };
const TypeName kTypes[] = {
    {"8UC1",CV_8UC1},{"8UC2",CV_8UC2},{"8UC3",CV_8UC3},{"8UC4",CV_8UC4},
    {"8SC1",CV_8SC1},{"8SC2",CV_8SC2},{"8SC3",CV_8SC3},{"8SC4",CV_8SC4},
    {"16UC1",CV_16UC1},{"16UC2",CV_16UC2},{"16UC3",CV_16UC3},{"16UC4",CV_16UC4},
    {"16SC1",CV_16SC1},{"16SC2",CV_16SC2},{"16SC3",CV_16SC3},{"16SC4",CV_16SC4},
    {"32SC1",CV_32SC1},{"32SC2",CV_32SC2},{"32SC3",CV_32SC3},{"32SC4",CV_32SC4},
    {"32FC1",CV_32FC1},{"32FC2",CV_32FC2},{"32FC3",CV_32FC3},{"32FC4",CV_32FC4},
    {"64FC1",CV_64FC1},{"64FC2",CV_64FC2},{"64FC3",CV_64FC3},{"64FC4",CV_64FC4},
};
int typeOf(const std::string& s) {
    if (s.empty()) return CV_8UC3;
    std::string t;
    for (char c : s) if (std::isalnum((unsigned char)c)) t.push_back((char)std::toupper((unsigned char)c));
    if (!t.empty() && t.find_first_not_of("0123456789") == std::string::npos)
        return atoi(t.c_str());                       // 也允许直接给 OpenCV 的整型(CV_8UC3 = 16)
    for (const auto& e : kTypes) if (t == e.name) return e.type;
    throw std::runtime_error("不认识的图像类型 '" + s + "' (例: 8UC1 / 8UC3 / 32FC1)");
}
const char* typeNameOf(int t) {
    for (const auto& e : kTypes) if (e.type == t) return e.name;
    return "?";
}

// —— 码表: 字符串 -> OpenCV 常量(经 normKey, 前缀/大小写不敏感) ——
int colorCodeOf(const std::string& s) {
    int n = 0;
    if (numericKey(s, n)) return n;                  // 直接给整型枚举(OpenCV.js 的 cv.COLOR_* 表)
    const std::string k = normKey(s);
    struct E { const char* k; int v; };
    static const E t[] = {
        {"bgr2gray",cv::COLOR_BGR2GRAY},{"rgb2gray",cv::COLOR_RGB2GRAY},{"gray2bgr",cv::COLOR_GRAY2BGR},
        {"gray2rgb",cv::COLOR_GRAY2RGB},{"bgr2rgb",cv::COLOR_BGR2RGB},{"rgb2bgr",cv::COLOR_RGB2BGR},
        {"bgr2hsv",cv::COLOR_BGR2HSV},{"hsv2bgr",cv::COLOR_HSV2BGR},{"rgb2hsv",cv::COLOR_RGB2HSV},{"hsv2rgb",cv::COLOR_HSV2RGB},
        {"bgr2hls",cv::COLOR_BGR2HLS},{"hls2bgr",cv::COLOR_HLS2BGR},{"rgb2hls",cv::COLOR_RGB2HLS},{"hls2rgb",cv::COLOR_HLS2RGB},
        // ⚠ LAB 这几个在 OpenCV 4.x 里只有 `Lab` 这个驼峰名(没有全大写别名), 4.10 实测
        {"bgr2lab",cv::COLOR_BGR2Lab},{"lab2bgr",cv::COLOR_Lab2BGR},{"rgb2lab",cv::COLOR_RGB2Lab},{"lab2rgb",cv::COLOR_Lab2RGB},
        {"bgr2yuv",cv::COLOR_BGR2YUV},{"yuv2bgr",cv::COLOR_YUV2BGR},{"rgb2yuv",cv::COLOR_RGB2YUV},{"yuv2rgb",cv::COLOR_YUV2RGB},
        {"bgr2ycrcb",cv::COLOR_BGR2YCrCb},{"ycrcb2bgr",cv::COLOR_YCrCb2BGR},{"rgb2ycrcb",cv::COLOR_RGB2YCrCb},
        {"bgr2xyz",cv::COLOR_BGR2XYZ},{"xyz2bgr",cv::COLOR_XYZ2BGR},
        {"bgra2bgr",cv::COLOR_BGRA2BGR},{"bgr2bgra",cv::COLOR_BGR2BGRA},
        {"bgra2gray",cv::COLOR_BGRA2GRAY},{"rgba2gray",cv::COLOR_RGBA2GRAY},
        {"rgba2bgr",cv::COLOR_RGBA2BGR},{"rgba2rgb",cv::COLOR_RGBA2RGB},{"rgb2bgra",cv::COLOR_RGB2BGRA},
        {"rgba2bgra",cv::COLOR_RGBA2BGRA},{"bgra2rgba",cv::COLOR_BGRA2RGBA},
        {"gray2bgra",cv::COLOR_GRAY2BGRA},{"gray2rgba",cv::COLOR_GRAY2RGBA},
        {"bayerbg2bgr",cv::COLOR_BayerBG2BGR},{"bayergb2bgr",cv::COLOR_BayerGB2BGR},
    };
    for (const auto& e : t) if (k == e.k) return e.v;
    throw std::runtime_error("不认识的 cvtColor 码 '" + s + "' (例: BGR2GRAY / GRAY2BGR / BGR2HSV / BGR2RGB)");
}
int interpOf(const std::string& s) {
    int n = 0;
    if (numericKey(s, n)) return n;
    const std::string k = normKey(s);
    if (k.empty() || k == "linear" || k == "default") return cv::INTER_LINEAR;
    if (k.find("nearest") != std::string::npos) return cv::INTER_NEAREST;
    if (k.find("cubic") != std::string::npos) return cv::INTER_CUBIC;
    if (k.find("area") != std::string::npos) return cv::INTER_AREA;
    if (k.find("lanczos") != std::string::npos) return cv::INTER_LANCZOS4;
    return cv::INTER_LINEAR;
}
int borderOf(const std::string& s) {
    int n = 0;
    if (numericKey(s, n)) return n;
    const std::string k = normKey(s);
    if (k.empty() || k.find("default") != std::string::npos) return cv::BORDER_DEFAULT;
    if (k.find("replicate") != std::string::npos) return cv::BORDER_REPLICATE;
    if (k.find("reflect101") != std::string::npos) return cv::BORDER_REFLECT101;
    if (k.find("reflect") != std::string::npos) return cv::BORDER_REFLECT;
    if (k.find("wrap") != std::string::npos) return cv::BORDER_WRAP;
    if (k.find("transparent") != std::string::npos) return cv::BORDER_TRANSPARENT;
    if (k.find("isolated") != std::string::npos) return cv::BORDER_ISOLATED;
    return cv::BORDER_CONSTANT;              // 'constant' / 给了 value
}
int morphOf(const std::string& s) {
    int n = 0;
    if (numericKey(s, n)) return n;
    const std::string k = normKey(s);
    if (k.find("open") != std::string::npos) return cv::MORPH_OPEN;
    if (k.find("close") != std::string::npos) return cv::MORPH_CLOSE;
    if (k.find("gradient") != std::string::npos) return cv::MORPH_GRADIENT;
    if (k.find("tophat") != std::string::npos) return cv::MORPH_TOPHAT;
    if (k.find("blackhat") != std::string::npos) return cv::MORPH_BLACKHAT;
    if (k.find("hitmiss") != std::string::npos) return cv::MORPH_HITMISS;
    if (k.find("erode") != std::string::npos) return cv::MORPH_ERODE;
    if (k.find("dilate") != std::string::npos) return cv::MORPH_DILATE;
    throw std::runtime_error("不认识的形态学操作 '" + s + "' (例: OPEN / CLOSE / GRADIENT / TOPHAT / BLACKHAT)");
}
int threshOf(const std::string& s) {
    int n = 0;
    if (numericKey(s, n)) return n;                  // 数值天然支持组合(THRESH_BINARY|THRESH_OTSU = 8)
    const std::string k = normKey(s);
    int t = 0;
    if (k.find("binaryinv") != std::string::npos) t |= cv::THRESH_BINARY_INV;
    else if (k.find("binary") != std::string::npos) t |= cv::THRESH_BINARY;
    else if (k.find("trunc") != std::string::npos) t |= cv::THRESH_TRUNC;
    else if (k.find("tozeroinv") != std::string::npos) t |= cv::THRESH_TOZERO_INV;
    else if (k.find("tozero") != std::string::npos) t |= cv::THRESH_TOZERO;
    else t |= cv::THRESH_BINARY;
    if (k.find("otsu") != std::string::npos) t |= cv::THRESH_OTSU;
    if (k.find("triangle") != std::string::npos) t |= cv::THRESH_TRIANGLE;
    return t;
}
int cmpOf(const std::string& s) {
    int n = 0;
    if (numericKey(s, n)) return n;
    const std::string k = normKey(s);
    if (k == "gt" || k == "greater") return cv::CMP_GT;
    if (k == "ge") return cv::CMP_GE;
    if (k == "lt" || k == "less") return cv::CMP_LT;
    if (k == "le") return cv::CMP_LE;
    if (k == "ne" || k == "notequal") return cv::CMP_NE;
    return cv::CMP_EQ;                       // 'eq' / 'equal'
}
int normOf(const std::string& s) {
    int n = 0;
    if (numericKey(s, n)) return n;
    const std::string k = normKey(s);
    if (k.empty()) return cv::NORM_L2;
    if (k == "inf") return cv::NORM_INF;
    if (k == "l1") return cv::NORM_L1;
    if (k == "l2") return cv::NORM_L2;
    if (k.find("l2sqr") != std::string::npos) return cv::NORM_L2SQR;
    if (k.find("hamming") != std::string::npos) return cv::NORM_HAMMING;
    if (k.find("minmax") != std::string::npos) return cv::NORM_MINMAX;
    return cv::NORM_L2;
}
int colormapOf(const std::string& s) {
    const std::string k = normKey(s);
    struct E { const char* k; int v; };
    static const E t[] = {
        {"jet",cv::COLORMAP_JET},{"hot",cv::COLORMAP_HOT},{"bone",cv::COLORMAP_BONE},{"winter",cv::COLORMAP_WINTER},
        {"rainbow",cv::COLORMAP_RAINBOW},{"ocean",cv::COLORMAP_OCEAN},{"summer",cv::COLORMAP_SUMMER},{"spring",cv::COLORMAP_SPRING},
        {"cool",cv::COLORMAP_COOL},{"hsv",cv::COLORMAP_HSV},{"pink",cv::COLORMAP_PINK},{"autumn",cv::COLORMAP_AUTUMN},
        {"parula",cv::COLORMAP_PARULA},{"magma",cv::COLORMAP_MAGMA},{"inferno",cv::COLORMAP_INFERNO},{"plasma",cv::COLORMAP_PLASMA},
        {"viridis",cv::COLORMAP_VIRIDIS},{"cividis",cv::COLORMAP_CIVIDIS},{"twilight",cv::COLORMAP_TWILIGHT},{"turbo",cv::COLORMAP_TURBO},
        {"deepgreen",cv::COLORMAP_DEEPGREEN},
    };
    for (const auto& e : t) if (k == e.k) return e.v;
    if (!k.empty() && k.find_first_not_of("0123456789") == std::string::npos) return atoi(k.c_str());
    return cv::COLORMAP_JET;
}
int tmOf(const std::string& s) {
    int n = 0;
    if (numericKey(s, n)) return n;
    const std::string k = normKey(s);
    if (k.find("sqdiffnormed") != std::string::npos) return cv::TM_SQDIFF_NORMED;
    if (k.find("sqdiff") != std::string::npos) return cv::TM_SQDIFF;
    if (k.find("ccorrnormed") != std::string::npos) return cv::TM_CCORR_NORMED;
    if (k.find("ccorr") != std::string::npos) return cv::TM_CCORR;
    if (k.find("ccoeffnormed") != std::string::npos) return cv::TM_CCOEFF_NORMED;
    if (k.find("ccoeff") != std::string::npos) return cv::TM_CCOEFF;
    return cv::TM_CCOEFF_NORMED;
}
int retrOf(const std::string& s) {
    int n = 0;
    if (numericKey(s, n)) return n;
    const std::string k = normKey(s);
    if (k.empty() || k == "external") return cv::RETR_EXTERNAL;
    if (k == "list") return cv::RETR_LIST;
    if (k == "ccomp") return cv::RETR_CCOMP;
    if (k == "tree") return cv::RETR_TREE;
    return cv::RETR_EXTERNAL;
}
int chainOf(const std::string& s) {
    int n = 0;
    if (numericKey(s, n)) return n;
    const std::string k = normKey(s);
    if (k.find("none") != std::string::npos) return cv::CHAIN_APPROX_NONE;
    if (k.find("tc89kcoss") != std::string::npos) return cv::CHAIN_APPROX_TC89_KCOS;
    if (k.find("tc89") != std::string::npos) return cv::CHAIN_APPROX_TC89_L1;
    return cv::CHAIN_APPROX_SIMPLE;          // 默认(也接 'simple' / 'approxsimple' 等)
}
int fontOf(const std::string& s) {
    int n = 0;
    if (numericKey(s, n)) return n;
    const std::string k = normKey(s);
    if (k.find("plain") != std::string::npos) return cv::FONT_HERSHEY_PLAIN;
    if (k.find("duplex") != std::string::npos) return cv::FONT_HERSHEY_DUPLEX;
    if (k.find("complex") != std::string::npos) return cv::FONT_HERSHEY_COMPLEX;
    if (k.find("triplex") != std::string::npos) return cv::FONT_HERSHEY_TRIPLEX;
    if (k.find("scriptsimplex") != std::string::npos) return cv::FONT_HERSHEY_SCRIPT_SIMPLEX;
    if (k.find("scriptcomplex") != std::string::npos) return cv::FONT_HERSHEY_SCRIPT_COMPLEX;
    return cv::FONT_HERSHEY_SIMPLEX;
}
int lineTypeOf(const Json::Value& p) {
    if (p.isMember("lineType") && p["lineType"].isNumeric()) return p["lineType"].asInt();
    const std::string k = normKey(strOf(p, "lineType", "8"));
    if (k == "4") return 4;
    if (k == "aa" || k == "anti" || k == "antialias") return cv::LINE_AA;
    return 8;
}
int ddepthOf(const Json::Value& p, int def) {
    if (!p.isMember("ddepth")) return def;
    if (p["ddepth"].isNumeric()) return p["ddepth"].asInt();
    return typeOf(p["ddepth"].asString());
}

} // namespace

// Impl: 句柄表 + 运算表(表驱动)
struct JZJSCvModule::Impl
{
    std::map<int, Mat> mats;
    int nextId = 1;
    std::map<std::string, std::function<Json::Value(const Json::Value&)>> ops;
    bool opsBuilt = false;

    int put(Mat m) { const int id = nextId++; mats[id] = std::move(m); return id; }
    Mat* find(int id) { auto it = mats.find(id); return it == mats.end() ? nullptr : &it->second; }
    Mat& need(int id) {
        Mat* m = find(id);
        if (!m) throw std::runtime_error("无效的图像句柄 " + std::to_string(id) + "（已释放? 或 id 记错了）");
        return *m;
    }
    // mats: [id,...] -> Mat 列表
    std::vector<Mat> collect(const Json::Value& p) {
        if (!p.isMember("mats") || !p["mats"].isArray() || p["mats"].size() < 1)
            throw std::runtime_error("要传 mats: [id1,id2,...]");
        std::vector<Mat> v;
        for (const auto& e : p["mats"]) v.push_back(need(e.asInt()));
        return v;
    }
    void buildOps();
};

// 一份轮廓(点集) -> 派生量: 面积/周长/圆度/包围盒/中心/minAreaRect/凸包/近似多边形。
// findContours 与 contourMetrics(给 OpenCV.js 风格的独立算子用) 共用, 免得各算一遍。
Json::Value contourMetricsJson(const std::vector<Point>& c, double epsRatio)
{
    Json::Value o;
    Json::Value pts(Json::arrayValue);
    for (const Point& pt : c) { Json::Value q(Json::arrayValue); q.append(pt.x); q.append(pt.y); pts.append(q); }
    o["points"] = pts;
    const double area = cv::contourArea(c);
    const double len = cv::arcLength(c, true);
    o["area"] = area;
    o["length"] = len;
    o["circularity"] = len > 0 ? (4 * CV_PI * area) / (len * len) : 0.0;   // 1.0 = 正圆
    const Rect br = cv::boundingRect(c);
    Json::Value rb; rb["x"] = br.x; rb["y"] = br.y; rb["w"] = br.width; rb["h"] = br.height;
    o["boundingRect"] = rb;
    Json::Value ctr(Json::arrayValue);
    ctr.append(br.x + br.width / 2.0); ctr.append(br.y + br.height / 2.0);
    o["center"] = ctr;
    if (c.size() >= 5) {                     // minAreaRect 要 ≥5 个点
        const cv::RotatedRect rr = cv::minAreaRect(c);
        Json::Value mr, rc(Json::arrayValue), rs(Json::arrayValue);
        rc.append(rr.center.x); rc.append(rr.center.y);
        rs.append(rr.size.width); rs.append(rr.size.height);
        mr["center"] = rc; mr["size"] = rs; mr["angle"] = rr.angle;
        o["minAreaRect"] = mr;
    }
    std::vector<Point> hull;
    cv::convexHull(c, hull);
    Json::Value hp(Json::arrayValue);
    for (const Point& pt : hull) { Json::Value q(Json::arrayValue); q.append(pt.x); q.append(pt.y); hp.append(q); }
    o["hull"] = hp;
    o["hullArea"] = cv::contourArea(hull);
    std::vector<Point> ap;
    cv::approxPolyDP(c, ap, epsRatio * len, true);
    Json::Value app(Json::arrayValue);
    for (const Point& pt : ap) { Json::Value q(Json::arrayValue); q.append(pt.x); q.append(pt.y); app.append(q); }
    o["approx"] = app;
    return o;
}

// —— 点集 <-> 轮廓 Mat(Nx1 CV_32SC2 / CV_32FC2), 与 OpenCV.js 的"轮廓就是 Mat"一致 ——
// 直接按裸指针写: 不依赖 Vec2i/Vec4i 这些别名(有的 OpenCV 版本/包含顺序下拿不到)
Mat pointsToMat(const std::vector<Point>& pts)
{
    Mat m((int)pts.size(), 1, CV_32SC2);
    int* d = (int*)m.data;
    for (size_t i = 0; i < pts.size(); ++i) { d[i * 2] = pts[i].x; d[i * 2 + 1] = pts[i].y; }
    return m;
}
Mat indicesToMat(const std::vector<int>& idx)
{
    Mat m((int)idx.size(), 1, CV_32S);
    int* d = (int*)m.data;
    for (size_t i = 0; i < idx.size(); ++i) d[i] = idx[i];
    return m;
}
// 只吃 2 通道 32S/32F(OpenCV.js 的 findContours 就是这两种); 别的类型宁可报错也不猜
std::vector<Point> matToPoints(const Mat& m)
{
    std::vector<Point> out;
    if (m.empty()) return out;
    if (m.channels() != 2 || (m.depth() != CV_32S && m.depth() != CV_32F))
        throw std::runtime_error("轮廓 Mat 应该是 2 通道的 32SC2/32FC2, 现在是 "
                                 + std::to_string(m.channels()) + " 通道 depth " + std::to_string(m.depth()));
    const int n = m.rows * m.cols;
    if (m.depth() == CV_32S) {
        const int* d = (const int*)m.data;
        for (int i = 0; i < n; ++i) out.push_back(Point(d[i * 2], d[i * 2 + 1]));
    } else {
        const float* d = (const float*)m.data;
        for (int i = 0; i < n; ++i) out.push_back(Point(cvRound(d[i * 2]), cvRound(d[i * 2 + 1])));
    }
    return out;
}

// —— 具体运算 ——
void JZJSCvModule::Impl::buildOps()
{
    if (opsBuilt) return;
    opsBuilt = true;

    auto M = [this](const Json::Value& p, const char* k) -> Mat& { return need(matIdOf(p, k)); };
    // 每个 op 的输出都是**新句柄**(不就地改输入): 链式处理时输入还能复用/对比。
    auto store = [this](Mat&& m) { return jOkMat(put(std::move(m))); };
    // 核参数: 既收"核描述"(数字 n => n×n 全 1 / [[..]] / {data} / {shape,ksize}), 也收**图像句柄** ——
    // OpenCV.js 的写法是 `k = cv.getStructuringElement(...)` 再把它当核传进来; 之前数字会被当成
    // "n×n 全 1 核", 于是把句柄 id(如 6)当成了 6×6 核, **静默**算错(实测形态学 628ms、轮廓全没了)。
    auto K = [this](const Json::Value& p, const char* k) -> Mat {
        if (p.isMember(k)) {
            const Json::Value& v = p[k];
            const int id = v.isNumeric() ? v.asInt() : ((v.isObject() && v.isMember("id")) ? v["id"].asInt() : 0);
            if (id > 0) {
                Mat* m = find(id);
                if (m && !m->empty()) return *m;
            }
        }
        return kernelOf(p, k);
    };

    // ———————————————— 颜色转换 / 通道 ————————————————
    ops["cvtColor"] = [M, store](const Json::Value& p) {
        Mat dst; cv::cvtColor(M(p, "src"), dst, colorCodeOf(strNumOf(p, "code", "BGR2GRAY")));
        return store(std::move(dst));
    };
    ops["inRange"] = [M, store](const Json::Value& p) {
        Mat dst; cv::inRange(M(p, "src"), scalarOf(p, "lower", Scalar(0, 0, 0)),
                             scalarOf(p, "upper", Scalar(255, 255, 255)), dst);
        return store(std::move(dst));
    };
    ops["convertTo"] = [M, store](const Json::Value& p) {
        Mat dst; M(p, "src").convertTo(dst, ddepthOf(p, -1), numOf(p, "alpha", 1.0), numOf(p, "beta", 0.0));
        return store(std::move(dst));
    };
    ops["split"] = [this, M](const Json::Value& p) {
        std::vector<Mat> ch; cv::split(M(p, "src"), ch);
        std::vector<int> ids;
        if (p.isMember("channels") && p["channels"].isArray()) {
            for (const auto& c : p["channels"]) {
                const int i = c.asInt();
                if (i >= 0 && i < (int)ch.size()) ids.push_back(put(ch[i]));
            }
        } else {
            for (auto& c : ch) ids.push_back(put(c));
        }
        return jOkMats(ids);
    };
    ops["merge"] = [this, store](const Json::Value& p) {
        std::vector<Mat> ch = collect(p);
        for (auto& c : ch) c = c.clone();            // merge 要求同尺寸同类型且连续
        Mat dst; cv::merge(ch, dst);
        return store(std::move(dst));
    };
    ops["lut"] = [M, store](const Json::Value& p) {
        if (!p.isMember("lut") || !p["lut"].isArray()) throw std::runtime_error("lut 要传 lut: [各灰度 → 目标值]");
        const Json::Value& l = p["lut"];
        Mat lut((int)l.size(), 1, CV_8U);
        for (Json::Value::ArrayIndex i = 0; i < l.size(); ++i)
            lut.at<uchar>((int)i, 0) = (uchar)std::max(0.0, std::min(255.0, l[i].asDouble()));
        Mat dst; cv::LUT(M(p, "src"), lut, dst);
        return store(std::move(dst));
    };
    ops["applyColorMap"] = [M, store](const Json::Value& p) {
        Mat dst; cv::applyColorMap(M(p, "src"), dst, colormapOf(strNumOf(p, "colormap", "jet")));
        return store(std::move(dst));
    };
    ops["equalizeHist"] = [M, store](const Json::Value& p) {
        Mat dst; cv::equalizeHist(toGray(M(p, "src")), dst);
        return store(std::move(dst));
    };
    ops["clahe"] = [M, store](const Json::Value& p) {
        Mat dst;
        auto cl = cv::createCLAHE(numOf(p, "clipLimit", 40.0), sizeOf(p, "tileGridSize", Size(8, 8)));
        cl->apply(toGray(M(p, "src")), dst);
        return store(std::move(dst));
    };

    // ———————————————— 阈值 ————————————————
    ops["threshold"] = [this, M](const Json::Value& p) {
        Mat dst;
        const double t = cv::threshold(M(p, "src"), dst, numOf(p, "thresh", 0), numOf(p, "maxval", 255),
                                       threshOf(strNumOf(p, "type", "BINARY")));
        Json::Value v = jOkMat(put(std::move(dst)));
        v["thresh"] = t;                     // OTSU / TRIANGLE 会自己算, 把算出来的还回去
        return v;
    };
    ops["adaptiveThreshold"] = [M, store](const Json::Value& p) {
        Mat dst;
        const int method = lower(strOf(p, "method", "mean")).find("gauss") != std::string::npos
                         ? cv::ADAPTIVE_THRESH_GAUSSIAN_C : cv::ADAPTIVE_THRESH_MEAN_C;
        const int type = lower(strOf(p, "type", "binary")).find("inv") != std::string::npos
                       ? cv::THRESH_BINARY_INV : cv::THRESH_BINARY;
        int block = intOf(p, "blockSize", 11);
        if (block % 2 == 0) block += 1;      // 必须奇数
        cv::adaptiveThreshold(toGray(M(p, "src")), dst, numOf(p, "maxValue", 255), method, type,
                              block, numOf(p, "C", 2.0));
        return store(std::move(dst));
    };

    // ———————————————— 滤波 / 边缘 ————————————————
    ops["blur"] = [M, store](const Json::Value& p) {
        Mat dst; cv::blur(M(p, "src"), dst, sizeOf(p, "ksize", Size(3, 3)), pointOf(p, "anchor", Point(-1, -1)),
                          borderOf(strNumOf(p, "borderType", "default")));
        return store(std::move(dst));
    };
    ops["boxFilter"] = [M, store](const Json::Value& p) {
        Mat dst; cv::boxFilter(M(p, "src"), dst, ddepthOf(p, -1), sizeOf(p, "ksize", Size(3, 3)),
                               pointOf(p, "anchor", Point(-1, -1)), boolOf(p, "normalize", true),
                               borderOf(strNumOf(p, "borderType", "default")));
        return store(std::move(dst));
    };
    ops["gaussianBlur"] = [M, store](const Json::Value& p) {
        Mat dst; cv::GaussianBlur(M(p, "src"), dst, sizeOf(p, "ksize", Size(5, 5)),
                                  numOf(p, "sigmaX", 0), numOf(p, "sigmaY", 0),
                                  borderOf(strNumOf(p, "borderType", "default")));
        return store(std::move(dst));
    };
    ops["medianBlur"] = [M, store](const Json::Value& p) {
        Mat dst; int k = intOf(p, "ksize", 5);
        cv::medianBlur(M(p, "src"), dst, k % 2 == 0 ? k + 1 : k);       // 核必须是奇数
        return store(std::move(dst));
    };
    ops["bilateralFilter"] = [M, store](const Json::Value& p) {
        Mat dst; cv::bilateralFilter(M(p, "src"), dst, intOf(p, "d", 5),
                                     numOf(p, "sigmaColor", 50), numOf(p, "sigmaSpace", 50),
                                     borderOf(strNumOf(p, "borderType", "default")));
        return store(std::move(dst));
    };
    ops["filter2D"] = [M, store, K](const Json::Value& p) {
        Mat k = K(p, "kernel");
        if (k.empty()) throw std::runtime_error("filter2D 要传 kernel: [[..],[..]] 或 {data:[[..]]}");
        Mat dst; cv::filter2D(M(p, "src"), dst, ddepthOf(p, -1), k,
                              pointOf(p, "anchor", Point(-1, -1)), numOf(p, "delta", 0),
                              borderOf(strNumOf(p, "borderType", "default")));
        return store(std::move(dst));
    };
    ops["sobel"] = [M, store](const Json::Value& p) {
        Mat dst; cv::Sobel(M(p, "src"), dst, ddepthOf(p, CV_16S), intOf(p, "dx", 1), intOf(p, "dy", 0),
                           intOf(p, "ksize", 3), numOf(p, "scale", 1), numOf(p, "delta", 0),
                           borderOf(strNumOf(p, "borderType", "default")));
        return store(std::move(dst));
    };
    ops["scharr"] = [M, store](const Json::Value& p) {
        Mat dst; cv::Scharr(M(p, "src"), dst, ddepthOf(p, CV_16S), intOf(p, "dx", 1), intOf(p, "dy", 0),
                            numOf(p, "scale", 1), numOf(p, "delta", 0),
                            borderOf(strNumOf(p, "borderType", "default")));
        return store(std::move(dst));
    };
    ops["laplacian"] = [M, store](const Json::Value& p) {
        Mat dst; cv::Laplacian(M(p, "src"), dst, ddepthOf(p, CV_16S), intOf(p, "ksize", 1),
                               numOf(p, "scale", 1), numOf(p, "delta", 0),
                               borderOf(strNumOf(p, "borderType", "default")));
        return store(std::move(dst));
    };
    ops["canny"] = [M, store](const Json::Value& p) {
        Mat dst; cv::Canny(toGray(M(p, "src")), dst, numOf(p, "threshold1", 100), numOf(p, "threshold2", 200),
                           intOf(p, "apertureSize", 3), boolOf(p, "l2gradient", false));
        return store(std::move(dst));
    };

    // ———————————————— 形态学 ————————————————
    ops["getStructuringElement"] = [this](const Json::Value& p) {
        int shape = cv::MORPH_RECT;
        const std::string s = lower(strOf(p, "shape", "rect"));
        if (s.find("ellip") != std::string::npos) shape = cv::MORPH_ELLIPSE;
        else if (s.find("cross") != std::string::npos) shape = cv::MORPH_CROSS;
        return jOkMat(put(cv::getStructuringElement(shape, sizeOf(p, "ksize", Size(3, 3)))));
    };
    ops["erode"] = [M, store, K](const Json::Value& p) {
        Mat k = K(p, "kernel");
        if (k.empty()) k = cv::getStructuringElement(cv::MORPH_RECT, Size(3, 3));
        Mat dst; cv::erode(M(p, "src"), dst, k, pointOf(p, "anchor", Point(-1, -1)), intOf(p, "iterations", 1),
                           borderOf(strNumOf(p, "borderType", "default")), scalarOf(p, "borderValue", cv::morphologyDefaultBorderValue()));
        return store(std::move(dst));
    };
    ops["dilate"] = [M, store, K](const Json::Value& p) {
        Mat k = K(p, "kernel");
        if (k.empty()) k = cv::getStructuringElement(cv::MORPH_RECT, Size(3, 3));
        Mat dst; cv::dilate(M(p, "src"), dst, k, pointOf(p, "anchor", Point(-1, -1)), intOf(p, "iterations", 1),
                            borderOf(strNumOf(p, "borderType", "default")), scalarOf(p, "borderValue", cv::morphologyDefaultBorderValue()));
        return store(std::move(dst));
    };
    ops["morphologyEx"] = [M, store, K](const Json::Value& p) {
        Mat k = K(p, "kernel");
        if (k.empty()) k = cv::getStructuringElement(cv::MORPH_RECT, Size(3, 3));
        Mat dst; cv::morphologyEx(M(p, "src"), dst, morphOf(strNumOf(p, "op", "open")), k,
                                  pointOf(p, "anchor", Point(-1, -1)), intOf(p, "iterations", 1),
                                  borderOf(strNumOf(p, "borderType", "default")), scalarOf(p, "borderValue", cv::morphologyDefaultBorderValue()));
        return store(std::move(dst));
    };
    ops["connectedComponents"] = [this, M](const Json::Value& p) {
        Mat labels, stats, centroids;
        const int n = cv::connectedComponentsWithStats(M(p, "src"), labels, stats, centroids,
                                                       intOf(p, "connectivity", 8), CV_32S);
        Json::Value st(Json::arrayValue), ce(Json::arrayValue);
        for (int i = 0; i < n; ++i) {
            Json::Value row(Json::arrayValue);
            row.append(stats.at<int>(i, cv::CC_STAT_LEFT));
            row.append(stats.at<int>(i, cv::CC_STAT_TOP));
            row.append(stats.at<int>(i, cv::CC_STAT_WIDTH));
            row.append(stats.at<int>(i, cv::CC_STAT_HEIGHT));
            row.append(stats.at<int>(i, cv::CC_STAT_AREA));
            st.append(row);
            Json::Value c(Json::arrayValue);
            c.append(centroids.at<double>(i, 0)); c.append(centroids.at<double>(i, 1));
            ce.append(c);
        }
        Json::Value v = jOkMat(put(std::move(labels)));
        v["count"] = n; v["stats"] = st; v["centroids"] = ce;
        return v;
    };

    // ———————————————— 几何变换 ————————————————
    ops["resize"] = [M, store](const Json::Value& p) {
        Mat dst, src = M(p, "src");
        const int interp = interpOf(strNumOf(p, "interpolation", "linear"));
        if (p.isMember("fx") || p.isMember("fy"))
            cv::resize(src, dst, Size(), numOf(p, "fx", 1), numOf(p, "fy", 1), interp);
        else
            cv::resize(src, dst, sizeOf(p, "size", Size()), 0, 0, interp);
        return store(std::move(dst));
    };
    ops["crop"] = [M, store](const Json::Value& p) {
        Mat src = M(p, "src");
        Rect r = p.isMember("rect") ? rectOf(p["rect"])
                                    : Rect(intOf(p, "x", 0), intOf(p, "y", 0), intOf(p, "w", src.cols), intOf(p, "h", src.rows));
        r &= Rect(0, 0, src.cols, src.rows);         // 越界自动裁剪(脚本里常按比例取, 不抛更好用)
        if (r.width <= 0 || r.height <= 0) throw std::runtime_error("crop 的区域是空的");
        return store(src(r).clone());                // 拷贝: 不与原图共享像素
    };
    ops["flip"] = [M, store](const Json::Value& p) {
        Mat dst; int code = intOf(p, "code", -999);
        if (code == -999) {
            const std::string k = lower(strOf(p, "how", "horizontal"));
            code = (k.find("both") != std::string::npos) ? -1 : (k.find("vert") != std::string::npos ? 0 : 1);
        }
        cv::flip(M(p, "src"), dst, code);
        return store(std::move(dst));
    };
    ops["rotate"] = [M, store](const Json::Value& p) {
        Mat dst; int code = cv::ROTATE_90_CLOCKWISE;
        if (p.isMember("code")) {
            if (p["code"].isNumeric()) code = p["code"].asInt();
            else {
                const std::string k = normKey(p["code"].asString());
                if (k.find("180") != std::string::npos) code = cv::ROTATE_180;
                else if (k.find("counter") != std::string::npos) code = cv::ROTATE_90_COUNTERCLOCKWISE;
                else if (k.find("180") != std::string::npos) code = cv::ROTATE_180;
            }
        }
        cv::rotate(M(p, "src"), dst, code);
        return store(std::move(dst));
    };
    ops["transpose"] = [M, store](const Json::Value& p) {
        Mat dst; cv::transpose(M(p, "src"), dst);
        return store(std::move(dst));
    };
    ops["warpAffine"] = [M, store](const Json::Value& p) {
        if (!p.isMember("M") || !p["M"].isArray() || p["M"].size() == 0)
            throw std::runtime_error("warpAffine 要传 M: [[..],[..]] (2x3; 可用 getRotationMatrix2D 生成)");
        const Json::Value& mj = p["M"];
        Mat Mx((int)mj.size(), (int)mj[0u].size(), CV_64F);
        for (Json::Value::ArrayIndex y = 0; y < mj.size(); ++y)
            for (Json::Value::ArrayIndex x = 0; x < mj[y].size(); ++x)
                Mx.at<double>((int)y, (int)x) = mj[y][x].asDouble();
        Mat src = M(p, "src"), dst;
        cv::warpAffine(src, dst, Mx, sizeOf(p, "size", src.size()), interpOf(strNumOf(p, "interpolation", "linear")),
                       borderOf(strOf(p, "borderType", "constant")), scalarOf(p, "borderValue", Scalar(0, 0, 0)));
        return store(std::move(dst));
    };
    ops["warpPerspective"] = [M, store](const Json::Value& p) {
        if (!p.isMember("H") || !p["H"].isArray() || p["H"].size() == 0)
            throw std::runtime_error("warpPerspective 要传 H: [[..],[..],[..]] (3x3; 可用 getPerspectiveTransform 生成)");
        const Json::Value& hj = p["H"];
        Mat Hx((int)hj.size(), (int)hj[0u].size(), CV_64F);
        for (Json::Value::ArrayIndex y = 0; y < hj.size(); ++y)
            for (Json::Value::ArrayIndex x = 0; x < hj[y].size(); ++x)
                Hx.at<double>((int)y, (int)x) = hj[y][x].asDouble();
        Mat src = M(p, "src"), dst;
        cv::warpPerspective(src, dst, Hx, sizeOf(p, "size", src.size()), interpOf(strNumOf(p, "interpolation", "linear")),
                            borderOf(strOf(p, "borderType", "constant")), scalarOf(p, "borderValue", Scalar(0, 0, 0)));
        return store(std::move(dst));
    };
    ops["getRotationMatrix2D"] = [this](const Json::Value& p) {
        Mat m = cv::getRotationMatrix2D(pointOf(p, "center", Point(0, 0)), numOf(p, "angle", 0), numOf(p, "scale", 1));
        return jOkMat(put(m));
    };
    ops["getPerspectiveTransform"] = [this](const Json::Value& p) {
        if (!p.isMember("src") || !p.isMember("dst") || p["src"].size() != 4 || p["dst"].size() != 4)
            throw std::runtime_error("getPerspectiveTransform 要传 src/dst 各 4 个点: [[x,y],...]");
        std::vector<Point2f> s, d;
        for (int i = 0; i < 4; ++i) {
            s.push_back(pointOf(p["src"][(Json::Value::ArrayIndex)i]));
            d.push_back(pointOf(p["dst"][(Json::Value::ArrayIndex)i]));
        }
        return jOkMat(put(cv::getPerspectiveTransform(s, d)));
    };
    ops["copyMakeBorder"] = [M, store](const Json::Value& p) {
        Mat dst; cv::copyMakeBorder(M(p, "src"), dst, intOf(p, "top", 0), intOf(p, "bottom", 0),
                                    intOf(p, "left", 0), intOf(p, "right", 0),
                                    borderOf(strOf(p, "borderType", "constant")), scalarOf(p, "value", Scalar(0, 0, 0)));
        return store(std::move(dst));
    };
    ops["hconcat"] = [this, store](const Json::Value& p) {
        Mat dst; std::vector<Mat> v = collect(p); cv::hconcat(v, dst);
        return store(std::move(dst));
    };
    ops["vconcat"] = [this, store](const Json::Value& p) {
        Mat dst; std::vector<Mat> v = collect(p); cv::vconcat(v, dst);
        return store(std::move(dst));
    };

    // ———————————————— 算术 / 位运算 ————————————————
    auto bin = [M, store](const Json::Value& p, int kind) {
        Mat dst;
        switch (kind) {
        case 0: cv::add(M(p, "a"), M(p, "b"), dst); break;
        case 1: cv::subtract(M(p, "a"), M(p, "b"), dst); break;
        case 2: cv::multiply(M(p, "a"), M(p, "b"), dst, numOf(p, "scale", 1)); break;
        case 3: cv::divide(M(p, "a"), M(p, "b"), dst, numOf(p, "scale", 1)); break;
        case 4: cv::absdiff(M(p, "a"), M(p, "b"), dst); break;
        case 5: cv::bitwise_and(M(p, "a"), M(p, "b"), dst); break;
        case 6: cv::bitwise_or(M(p, "a"), M(p, "b"), dst); break;
        case 7: cv::bitwise_xor(M(p, "a"), M(p, "b"), dst); break;
        case 8: cv::min(M(p, "a"), M(p, "b"), dst); break;
        case 9: cv::max(M(p, "a"), M(p, "b"), dst); break;
        default: break;
        }
        return store(std::move(dst));
    };
    ops["add"] = [bin](const Json::Value& p) { return bin(p, 0); };
    ops["subtract"] = [bin](const Json::Value& p) { return bin(p, 1); };
    ops["multiply"] = [bin](const Json::Value& p) { return bin(p, 2); };
    ops["divide"] = [bin](const Json::Value& p) { return bin(p, 3); };
    ops["absdiff"] = [bin](const Json::Value& p) { return bin(p, 4); };
    ops["bitwiseAnd"] = [bin](const Json::Value& p) { return bin(p, 5); };
    ops["bitwiseOr"] = [bin](const Json::Value& p) { return bin(p, 6); };
    ops["bitwiseXor"] = [bin](const Json::Value& p) { return bin(p, 7); };
    ops["min"] = [bin](const Json::Value& p) { return bin(p, 8); };
    ops["max"] = [bin](const Json::Value& p) { return bin(p, 9); };
    ops["bitwiseNot"] = [M, store](const Json::Value& p) {
        Mat dst; cv::bitwise_not(M(p, "src"), dst);
        return store(std::move(dst));
    };
    ops["addWeighted"] = [M, store](const Json::Value& p) {
        Mat dst; cv::addWeighted(M(p, "a"), numOf(p, "alpha", 0.5), M(p, "b"), numOf(p, "beta", 0.5),
                                 numOf(p, "gamma", 0), dst);
        return store(std::move(dst));
    };
    ops["compare"] = [M, store](const Json::Value& p) {
        Mat& a = M(p, "a");
        Mat dst; const int op = cmpOf(strNumOf(p, "cmpop", "eq"));
        if (p.isMember("value") && !p.isMember("b")) {
            // ⚠ cv::compare 没有"与标量比"的重载(Scalar 会被当成 InputArray 用错)
            //   ⇒ 铺一张同尺寸同类型的常量图再比。小图无所谓, 大图也只多一份临时内存。
            Mat rhs(a.size(), a.type(), scalarOf(p, "value", Scalar(0, 0, 0)));
            cv::compare(a, rhs, dst, op);
        } else {
            cv::compare(a, M(p, "b"), dst, op);
        }
        return store(std::move(dst));
    };
    ops["normalize"] = [M, store](const Json::Value& p) {
        Mat dst; cv::normalize(M(p, "src"), dst, numOf(p, "alpha", 0), numOf(p, "beta", 255),
                               normOf(strNumOf(p, "normType", "minmax")), ddepthOf(p, -1));
        return store(std::move(dst));
    };

    // ———————————————— 统计 ————————————————
    ops["meanStdDev"] = [this, M](const Json::Value& p) {
        Mat mask;
        if (p.isMember("mask") && p["mask"].isNumeric()) mask = need(p["mask"].asInt());
        Mat mean, stddev; cv::meanStdDev(M(p, "src"), mean, stddev, mask);
        Json::Value mn(Json::arrayValue), sd(Json::arrayValue);
        for (int i = 0; i < (int)mean.total(); ++i) { mn.append(mean.at<double>(i)); sd.append(stddev.at<double>(i)); }
        Json::Value v = jOk(); v["mean"] = mn; v["stddev"] = sd;
        return v;
    };
    ops["minMaxLoc"] = [this, M](const Json::Value& p) {
        double lo = 0, hi = 0; Point lpt, hpt;
        Mat mask;
        if (p.isMember("mask") && p["mask"].isNumeric()) mask = need(p["mask"].asInt());
        cv::minMaxLoc(M(p, "src"), &lo, &hi, &lpt, &hpt, mask);
        Json::Value v = jOk();
        v["minVal"] = lo; v["maxVal"] = hi;
        Json::Value a(Json::arrayValue); a.append(lpt.x); a.append(lpt.y); v["minLoc"] = a;
        Json::Value b(Json::arrayValue); b.append(hpt.x); b.append(hpt.y); v["maxLoc"] = b;
        return v;
    };
    ops["countNonZero"] = [M](const Json::Value& p) {
        Json::Value v = jOk(); v["value"] = (Json::Int64)cv::countNonZero(M(p, "src")); return v;
    };
    ops["sum"] = [M](const Json::Value& p) {
        const Scalar s = cv::sum(M(p, "src"));
        Json::Value a(Json::arrayValue);
        a.append(s[0]); a.append(s[1]); a.append(s[2]); a.append(s[3]);
        return jOkVal(a);
    };
    ops["norm"] = [M](const Json::Value& p) {
        Json::Value v = jOk();
        v["value"] = p.isMember("b") ? cv::norm(M(p, "a"), M(p, "b"), normOf(strNumOf(p, "normType", "l2")))
                                     : cv::norm(M(p, "a"), normOf(strNumOf(p, "normType", "l2")));
        return v;
    };
    // cv.mean -> Scalar(各通道均值, 不足补 0) —— OpenCV.js 的 cv.mean(src[, mask]) 就是它
    ops["mean"] = [this, M](const Json::Value& p) {
        Mat mask;
        if (p.isMember("mask") && p["mask"].isNumeric()) mask = need(p["mask"].asInt());
        const Scalar s = cv::mean(M(p, "src"), mask);
        Json::Value a(Json::arrayValue);
        a.append(s[0]); a.append(s[1]); a.append(s[2]); a.append(s[3]);
        return jOkVal(a);
    };
    // Mat::dot —— 两个同尺寸同类型的 Mat 按行优先当向量做点积(回一个数)
    ops["dot"] = [M](const Json::Value& p) {
        Json::Value v = jOk(); v["value"] = M(p, "a").dot(M(p, "b")); return v;
    };
    // Mat::diag(d) —— 抽一条对角线: d=0 主对角, d>0 主对角下方, d<0 主对角上方(OpenCV 的约定)
    ops["diag"] = [M, store](const Json::Value& p) {
        Mat dst = M(p, "src").diag(intOf(p, "d", 0));      // MatExpr -> Mat
        return store(std::move(dst));
    };
    // cv::invert —— 矩阵求逆(默认 DECOMP_LU; 奇异时 OpenCV 给全 0 阵, 与 C++ 行为一致)
    ops["invert"] = [M, store](const Json::Value& p) {
        Mat dst;
        cv::invert(M(p, "src"), dst, intOf(p, "method", cv::DECOMP_LU));
        return store(std::move(dst));
    };

    // ———————————————— 轮廓 / 形状 ————————————————
    ops["findContours"] = [M](const Json::Value& p) {
        Mat work = toGray(M(p, "src")).clone();      // findContours 会改输入
        if (work.type() != CV_8UC1) work.convertTo(work, CV_8UC1);
        std::vector<std::vector<Point>> cs;
        cv::findContours(work, cs, retrOf(strNumOf(p, "mode", "external")), chainOf(strNumOf(p, "method", "simple")));
        const double eps = numOf(p, "approxEpsilon", 0.02);
        Json::Value arr(Json::arrayValue);
        for (const auto& c : cs) {
            if (c.empty()) continue;
            arr.append(contourMetricsJson(c, eps));
        }
        return jOkVal(arr);
    };
    // 独立轮廓算子: 点集 [[x,y],...] -> 与 findContours 的每一项同构的派生量。
    // TS 侧的 contourArea / arcLength / boundingRect / minAreaRect / approxPolyDP / convexHull
    // (OpenCV.js 形状) 都走这一条, 不在 JS 里重写一遍这些算法。
    ops["contourMetrics"] = [](const Json::Value& p) {
        const Json::Value& src = p.isMember("pts") ? p["pts"] : p["points"];
        if (!src.isArray() || src.size() < 1)
            throw std::runtime_error("contourMetrics 要传 pts: [[x,y],...]");
        std::vector<Point> c;
        for (const Json::Value& q : src) c.push_back(pointOf(q));
        return jOkVal(contourMetricsJson(c, numOf(p, "approxEpsilon", 0.02)));
    };

    // 轮廓点集入口: src(轮廓 Mat 的句柄) 或 pts(点数组) —— 上面那批"按需算子"共用
    auto PTS = [this](const Json::Value& p) -> std::vector<Point> {
        if (p.isMember("src") && p["src"].isNumeric())
            return matToPoints(need(p["src"].asInt()));
        const Json::Value& src = p.isMember("pts") ? p["pts"] : p["points"];
        if (src.isArray()) {
            std::vector<Point> v;
            for (const Json::Value& q : src) v.push_back(pointOf(q));
            return v;
        }
        throw std::runtime_error("要传 src(轮廓 Mat 的句柄) 或 pts: [[x,y],...]");
    };

    // ———— OpenCV.js 形状的轮廓: 轮廓就是**一张 Mat**(存在句柄表里), 一个点都不序列化 ————
    // findContoursMats: 与 findContours 同样的输入, 但把每条轮廓存成 Mat(Nx1 32SC2), 只回句柄 id;
    // 层次结构也是句柄。于是"找轮廓"这步不再有 JSON 负担(实测 27ms → 2ms 级), 派生量按需再问
    // (contourStat / convexHullOf / approxPolyDPOf / contourPointsOf)。
    ops["findContoursMats"] = [this, M](const Json::Value& p) {
        Mat work = toGray(M(p, "src")).clone();      // findContours 会改输入
        if (work.type() != CV_8UC1) work.convertTo(work, CV_8UC1);
        std::vector<std::vector<Point>> cs;
        Mat hier;                                     // 层次结构直接给 Mat(OpenCV 4 的常见用法, Nx1 CV_32SC4)
        cv::findContours(work, cs, hier, retrOf(strNumOf(p, "mode", "external")),
                         chainOf(strNumOf(p, "method", "simple")));
        Json::Value mats(Json::arrayValue);
        for (const auto& c : cs) mats.append(put(pointsToMat(c)));
        Json::Value v = jOk();
        v["mats"] = mats;
        v["hierarchy"] = put(std::move(hier));
        return v;
    };
    // 按需取一个量: what = area|length|circularity|convex|hullarea|points|bbox|center|minarea
    // (只回数字/小对象 —— OpenCV.js 就是这么按需算的, 不把点搬来搬去)
    ops["contourStat"] = [PTS](const Json::Value& p) {
        const std::vector<Point> c = PTS(p);
        const std::string what = lower(strOf(p, "what", "area"));
        Json::Value v = jOk();
        if (what == "area") v["value"] = cv::contourArea(c);
        else if (what == "length") v["value"] = cv::arcLength(c, boolOf(p, "closed", true));
        else if (what == "points") v["value"] = (int)c.size();
        else if (what == "convex") v["value"] = cv::isContourConvex(c);
        else if (what == "circularity") {
            const double a = cv::contourArea(c), l = cv::arcLength(c, true);
            v["value"] = l > 0 ? (4 * CV_PI * a) / (l * l) : 0.0;
        } else if (what == "hullarea") {
            std::vector<Point> h; cv::convexHull(c, h); v["value"] = cv::contourArea(h);
        } else if (what == "bbox") {
            const Rect r = cv::boundingRect(c);          // 键名与 OpenCV.js 的 cv.Rect 一致
            Json::Value o; o["x"] = r.x; o["y"] = r.y; o["width"] = r.width; o["height"] = r.height;
            v["value"] = o;
        } else if (what == "center") {
            const Rect r = cv::boundingRect(c);
            Json::Value a(Json::arrayValue);
            a.append(r.x + r.width / 2.0); a.append(r.y + r.height / 2.0);
            v["value"] = a;
        } else if (what == "minarea") {
            const cv::RotatedRect rr = cv::minAreaRect(c);
            Json::Value o, ctr, sz;
            ctr["x"] = rr.center.x; ctr["y"] = rr.center.y;
            sz["width"] = rr.size.width; sz["height"] = rr.size.height;
            o["center"] = ctr; o["size"] = sz; o["angle"] = rr.angle;   // 键名与 cv.RotatedRect 一致
            v["value"] = o;
        } else
            throw std::runtime_error("contourStat 不认识的 what '" + what + "'");
        return v;
    };
    // 凸包 / 多边形近似: **出参也是一张 Mat**(与 OpenCV.js 的 cv.convexHull(cnt, hull) 一致)
    ops["convexHullOf"] = [this, PTS](const Json::Value& p) {
        const std::vector<Point> c = PTS(p);
        const bool returnPoints = boolOf(p, "returnPoints", true);
        if (returnPoints) {
            std::vector<Point> h;
            cv::convexHull(c, h, boolOf(p, "clockwise", false), true);
            return jOkMat(put(pointsToMat(h)));
        }
        std::vector<int> idx;
        cv::convexHull(c, idx, boolOf(p, "clockwise", false), false);
        return jOkMat(put(indicesToMat(idx)));
    };
    ops["approxPolyDPOf"] = [this, PTS](const Json::Value& p) {
        std::vector<Point> ap;
        cv::approxPolyDP(PTS(p), ap, numOf(p, "eps", 2.0), boolOf(p, "closed", true));
        return jOkMat(put(pointsToMat(ap)));
    };
    // 取点集(要"看/算几何"时才走这条, 会把点序列化成 JSON —— 明确是 opt-in)
    ops["contourPointsOf"] = [PTS](const Json::Value& p) {
        const std::vector<Point> c = PTS(p);
        Json::Value pts(Json::arrayValue);
        for (const Point& pt : c) { Json::Value q(Json::arrayValue); q.append(pt.x); q.append(pt.y); pts.append(q); }
        return jOkVal(pts);
    };
    ops["moments"] = [M](const Json::Value& p) {
        const cv::Moments m = cv::moments(M(p, "src"), boolOf(p, "binary", false));
        Json::Value v = jOk();
        v["m00"] = m.m00; v["m10"] = m.m10; v["m01"] = m.m01;
        v["m20"] = m.m20; v["m11"] = m.m11; v["m02"] = m.m02;
        v["m30"] = m.m30; v["m21"] = m.m21; v["m12"] = m.m12; v["m03"] = m.m03;
        v["nu20"] = m.nu20; v["nu11"] = m.nu11; v["nu02"] = m.nu02; v["nu30"] = m.nu30;
        // ⚠ 4.10 的 cv::Moments 没有 hu 成员(只有 m*/mu*/nu*), 得走 cv::HuMoments 算
        double huArr[7] = { 0, 0, 0, 0, 0, 0, 0 };
        cv::HuMoments(m, huArr);
        Json::Value hu(Json::arrayValue);
        for (int i = 0; i < 7; ++i) hu.append(huArr[i]);
        v["hu"] = hu;
        return v;
    };
    ops["matchTemplate"] = [this, M](const Json::Value& p) {
        Mat res;
        cv::matchTemplate(M(p, "src"), M(p, "templ"), res, tmOf(strNumOf(p, "method", "ccoeff_normed")));
        double lo = 0, hi = 0; Point lpt, hpt;
        cv::minMaxLoc(res, &lo, &hi, &lpt, &hpt);
        Json::Value v = jOkMat(put(std::move(res)));
        v["minVal"] = lo; v["maxVal"] = hi;
        Json::Value a(Json::arrayValue); a.append(lpt.x); a.append(lpt.y); v["minLoc"] = a;
        Json::Value b(Json::arrayValue); b.append(hpt.x); b.append(hpt.y); v["maxLoc"] = b;
        return v;
    };
    ops["calcHist"] = [this, M](const Json::Value& p) {
        Mat src = M(p, "src");
        const int ch = intOf(p, "channel", 0);
        const int bins = intOf(p, "bins", 256);
        const float lo = (float)numOf(p, "min", 0), hi = (float)numOf(p, "max", 256);
        int chs[] = { ch };
        int sz[] = { bins };
        float rng[] = { lo, hi };
        const float* ranges[] = { rng };
        Mat h;
        // ⚠ uniform 必须给 true: 非均匀直方图要求 ranges 给 bins+1 个边界值, 只给 [lo,hi)
        //   会被当成"边界表"越界读(实测 assert ranges[i][k] < ranges[i][k+1] 失败)
        cv::calcHist(&src, 1, chs, Mat(), h, 1, sz, ranges, true);
        Json::Value counts(Json::arrayValue);
        for (int i = 0; i < bins; ++i) counts.append(h.at<float>(i));
        Json::Value v = jOkMat(put(std::move(h)));
        v["counts"] = counts;
        return v;
    };

    // ———————————————— 绘制(画在 Mat 上, 便于可视化/调试) ————————————————
    ops["line"] = [M, store](const Json::Value& p) {
        Mat dst = M(p, "src").clone();
        cv::line(dst, pointOf(p, "pt1"), pointOf(p, "pt2"), scalarOf(p, "color", Scalar(0, 0, 255)),
                 intOf(p, "thickness", 1), lineTypeOf(p), intOf(p, "shift", 0));
        return store(std::move(dst));
    };
    ops["rectangle"] = [M, store](const Json::Value& p) {
        Mat dst = M(p, "src").clone();
        const Rect r = p.isMember("rect") ? rectOf(p["rect"]) : Rect(pointOf(p, "pt1"), pointOf(p, "pt2"));
        cv::rectangle(dst, r, scalarOf(p, "color", Scalar(0, 0, 255)), intOf(p, "thickness", 1), lineTypeOf(p));
        return store(std::move(dst));
    };
    ops["circle"] = [M, store](const Json::Value& p) {
        Mat dst = M(p, "src").clone();
        cv::circle(dst, pointOf(p, "center"), intOf(p, "radius", 5), scalarOf(p, "color", Scalar(0, 0, 255)),
                   intOf(p, "thickness", 1), lineTypeOf(p));
        return store(std::move(dst));
    };
    ops["ellipse"] = [M, store](const Json::Value& p) {
        Mat dst = M(p, "src").clone();
        cv::ellipse(dst, pointOf(p, "center"), sizeOf(p, "axes", Size(10, 10)), numOf(p, "angle", 0),
                    numOf(p, "startAngle", 0), numOf(p, "endAngle", 360), scalarOf(p, "color", Scalar(0, 0, 255)),
                    intOf(p, "thickness", 1), lineTypeOf(p));
        return store(std::move(dst));
    };
    ops["polylines"] = [M, store](const Json::Value& p) {
        if (!p.isMember("pts") || !p["pts"].isArray()) throw std::runtime_error("polylines 要传 pts: [[x,y],...]");
        std::vector<Point> v;
        for (const auto& q : p["pts"]) v.push_back(pointOf(q));
        Mat dst = M(p, "src").clone();
        const Point* ppt[] = { v.data() };
        int n[] = { (int)v.size() };
        cv::polylines(dst, ppt, n, 1, boolOf(p, "closed", true), scalarOf(p, "color", Scalar(0, 0, 255)),
                      intOf(p, "thickness", 1), lineTypeOf(p));
        return store(std::move(dst));
    };
    ops["putText"] = [M, store](const Json::Value& p) {
        Mat dst = M(p, "src").clone();
        // ⚠ OpenCV 内置字体只有 ASCII: 中文会画成 '???'。要中文走 JZPainter(控件绘制那条路)。
        cv::putText(dst, strOf(p, "text", ""), pointOf(p, "org"), fontOf(strNumOf(p, "fontFace", "simplex")),
                    numOf(p, "fontScale", 1), scalarOf(p, "color", Scalar(0, 255, 0)),
                    intOf(p, "thickness", 1), lineTypeOf(p), boolOf(p, "bottomLeftOrigin", false));
        return store(std::move(dst));
    };

    // ========================================================================
    // 传统算法补充(2026-10-03): 逐元素数学 / 金字塔 / 统计 / 检测分割 / 轮廓形状 /
    // 直方图 / 几何去噪光流。口径与上面一致: 不改输入、输出新句柄, 码表照样"名字或数字都收"。
    // ========================================================================
    struct Code { const char* k; int v; };
    // 小枚举族: 先试数字直通(TS 侧给的是 cv.DIST_L2 这种整型), 再按子串匹配名字(忽略前缀大小写)
    auto tableOf = [](const std::string& s, const Code* t, size_t n, int def) -> int {
        if (s.empty()) return def;
        int num = 0;
        if (numericKey(s, num)) return num;
        const std::string k = normKey(s);
        for (size_t i = 0; i < n; ++i) if (k.find(t[i].k) != std::string::npos) return t[i].v;
        return def;
    };
    static const Code kReduceOp[]   = {{"sum",cv::REDUCE_SUM},{"avg",cv::REDUCE_AVG},{"max",cv::REDUCE_MAX},{"min",cv::REDUCE_MIN}};
    static const Code kDistType[]   = {{"l2",cv::DIST_L2},{"l1",cv::DIST_L1},{"c",cv::DIST_C}};   // "precise" 是 maskSize 那边的词
    static const Code kDistMask[]   = {{"precise",cv::DIST_MASK_PRECISE},{"mask5",cv::DIST_MASK_5},{"mask3",cv::DIST_MASK_3},{"5",cv::DIST_MASK_5},{"3",cv::DIST_MASK_3}};
    static const Code kDistLabel[]  = {{"ccomp",cv::DIST_LABEL_CCOMP},{"pixel",cv::DIST_LABEL_PIXEL}};
    static const Code kHough[]      = {{"alt",cv::HOUGH_GRADIENT_ALT},{"multi",cv::HOUGH_MULTI_SCALE},{"prob",cv::HOUGH_PROBABILISTIC},{"standard",cv::HOUGH_STANDARD},{"gradient",cv::HOUGH_GRADIENT}};
    static const Code kKmeans[]     = {{"pp",cv::KMEANS_PP_CENTERS},{"initial",cv::KMEANS_USE_INITIAL_LABELS},{"random",cv::KMEANS_RANDOM_CENTERS}};
    static const Code kHistCmp[]    = {{"correl",cv::HISTCMP_CORREL},{"chisqr",cv::HISTCMP_CHISQR},{"intersect",cv::HISTCMP_INTERSECT},{"bhattacharyya",cv::HISTCMP_BHATTACHARYYA},{"hellinger",cv::HISTCMP_HELLINGER},{"kl",5}};
    static const Code kShapeMatch[] = {{"i1",cv::CONTOURS_MATCH_I1},{"i2",cv::CONTOURS_MATCH_I2},{"i3",cv::CONTOURS_MATCH_I3}};
    static const Code kInpaint[]    = {{"ns",1},{"telea",0}};   // photo 模块没编进来, 仅留码表给将来
    (void)kInpaint;
    static const Code kFitLine[]    = {{"l12",cv::DIST_L12},{"fair",cv::DIST_FAIR},{"welsch",cv::DIST_WELSCH},{"huber",cv::DIST_HUBER},{"l1",cv::DIST_L1},{"l2",cv::DIST_L2}};
    static const Code kHomography[] = {{"lmeds",cv::LMEDS},{"rho",cv::RHO},{"ransac",cv::RANSAC}};
    // 点集参数: {pts:[[x,y],...]} / {pts:<Mat 句柄>} / {contour:<Mat 句柄>} 都收
    auto ptsOf = [this](const Json::Value& v) -> std::vector<Point2f> {
        std::vector<Point2f> out;
        if (v.isNumeric()) {
            for (const Point& q : matToPoints(need(v.asInt()))) out.push_back(Point2f((float)q.x, (float)q.y));
            return out;
        }
        if (!v.isArray()) return out;
        for (const auto& q : v) {
            if (q.isArray() && q.size() >= 2) out.push_back(Point2f((float)q[0u].asDouble(), (float)q[1u].asDouble()));
        }
        return out;
    };
    auto ptsF = [ptsOf](const Json::Value& p) -> std::vector<Point2f> {
        if (p.isMember("pts")) return ptsOf(p["pts"]);
        if (p.isMember("contour")) return ptsOf(p["contour"]);
        return std::vector<Point2f>();
    };
    auto termOf = [](const Json::Value& p) {
        // ⚠ criteria 必须是**对象**才算: 传了 null/数字(比如常量没定义 -> NaN -> JSON null)时别去 isMember,
        //   JsonCpp 会直接抛 "requires objectValue or nullValue"
        const Json::Value& c = (p.isMember("criteria") && p["criteria"].isObject()) ? p["criteria"] : p;
        return cv::TermCriteria(intOf(c, "type", cv::TermCriteria::EPS | cv::TermCriteria::MAX_ITER),
                                intOf(c, "maxCount", 10), numOf(c, "epsilon", 1.0));
    };

    // —— (1) 逐元素数学(梯度幅值/相位、幂指对: 传统流程里做增强/归一化常用) ——
    ops["sqrt"] = [M, store](const Json::Value& p) { Mat d; cv::sqrt(M(p, "src"), d); return store(std::move(d)); };
    ops["exp"]  = [M, store](const Json::Value& p) { Mat d; cv::exp(M(p, "src"), d); return store(std::move(d)); };
    ops["log"]  = [M, store](const Json::Value& p) { Mat d; cv::log(M(p, "src"), d); return store(std::move(d)); };
    ops["pow"]  = [M, store](const Json::Value& p) { Mat d; cv::pow(M(p, "src"), numOf(p, "power", 2), d); return store(std::move(d)); };
    ops["magnitude"] = [M, store](const Json::Value& p) { Mat d; cv::magnitude(M(p, "a"), M(p, "b"), d); return store(std::move(d)); };
    ops["phase"] = [M, store](const Json::Value& p) {
        Mat d; cv::phase(M(p, "a"), M(p, "b"), d, boolOf(p, "degrees", false)); return store(std::move(d));
    };
    ops["cartToPolar"] = [M, this](const Json::Value& p) {
        Mat mag, ang; cv::cartToPolar(M(p, "a"), M(p, "b"), mag, ang, boolOf(p, "degrees", false));
        return jOkMats({ put(std::move(mag)), put(std::move(ang)) });
    };
    ops["polarToCart"] = [M, this](const Json::Value& p) {
        Mat x, y; cv::polarToCart(M(p, "mag"), M(p, "angle"), x, y, boolOf(p, "degrees", false));
        return jOkMats({ put(std::move(x)), put(std::move(y)) });
    };

    // —— (2) 图像金字塔 ——
    ops["pyrDown"] = [M, store](const Json::Value& p) {
        Mat d; cv::pyrDown(M(p, "src"), d, p.isMember("dstsize") ? sizeOf(p, "dstsize", Size()) : Size());
        return store(std::move(d));
    };
    ops["pyrUp"] = [M, store](const Json::Value& p) {
        Mat d; cv::pyrUp(M(p, "src"), d, p.isMember("dstsize") ? sizeOf(p, "dstsize", Size()) : Size());
        return store(std::move(d));
    };

    // —— (3) 统计 / 积分图 / 仿射 ——
    ops["reduce"] = [M, store, tableOf](const Json::Value& p) {
        Mat d; cv::reduce(M(p, "src"), d, intOf(p, "dim", 0),
                          tableOf(strNumOf(p, "op", "sum"), kReduceOp, sizeof(kReduceOp) / sizeof(Code), cv::REDUCE_SUM),
                          intOf(p, "dtype", -1));
        return store(std::move(d));
    };
    ops["findNonZero"] = [M, store](const Json::Value& p) {
        Mat d; cv::findNonZero(M(p, "src"), d); return store(std::move(d));    // Nx1 CV_32SC2
    };
    ops["integral"] = [M, this](const Json::Value& p) {
        const Mat& src = M(p, "src");
        const int sd = intOf(p, "sdepth", -1);
        std::vector<int> ids;
        if (boolOf(p, "sqsum", false) || boolOf(p, "tilted", false)) {
            Mat sum, sq, tilt;
            cv::integral(src, sum, sq, tilt, sd);
            ids.push_back(put(std::move(sum)));
            if (boolOf(p, "sqsum", false)) ids.push_back(put(std::move(sq)));
            if (boolOf(p, "tilted", false)) ids.push_back(put(std::move(tilt)));
        } else {
            Mat sum; cv::integral(src, sum, sd);
            ids.push_back(put(std::move(sum)));
        }
        return jOkMats(ids);
    };
    ops["getAffineTransform"] = [store, ptsOf](const Json::Value& p) {
        if (!p.isMember("src") || !p.isMember("dst")) throw std::runtime_error("getAffineTransform 要传 src/dst(各 3 个点)");
        const std::vector<Point2f> s = ptsOf(p["src"]), d = ptsOf(p["dst"]);
        if (s.size() != 3 || d.size() != 3) throw std::runtime_error("getAffineTransform 每组要**正好 3 个点**");
        Mat m = cv::getAffineTransform(s, d);
        return store(std::move(m));
    };

    // —— (4) 检测 / 分割 ——
    ops["HoughCircles"] = [M, store, tableOf](const Json::Value& p) {
        Mat d;
        cv::HoughCircles(toGray(M(p, "src")), d, tableOf(strNumOf(p, "method", "gradient"), kHough, sizeof(kHough) / sizeof(Code), cv::HOUGH_GRADIENT),
                         numOf(p, "dp", 1), numOf(p, "minDist", 10), numOf(p, "param1", 100),
                         numOf(p, "param2", 100), intOf(p, "minRadius", 0), intOf(p, "maxRadius", 0));
        return store(std::move(d));     // 1xN CV_32FC3: (x, y, r) —— 没找到就是空图
    };
    ops["HoughLinesP"] = [M, store](const Json::Value& p) {
        Mat d;
        cv::HoughLinesP(toGray(M(p, "src")), d, numOf(p, "rho", 1), numOf(p, "theta", CV_PI / 180),
                        intOf(p, "threshold", 50), numOf(p, "minLineLength", 50), numOf(p, "maxLineGap", 10));
        return store(std::move(d));     // Nx1 CV_32SC4: (x1, y1, x2, y2)
    };
    ops["distanceTransform"] = [M, this, tableOf](const Json::Value& p) {
        const Mat g = toGray(M(p, "src"));
        const int dt = tableOf(strNumOf(p, "distanceType", "l2"), kDistType, sizeof(kDistType) / sizeof(Code), cv::DIST_L2);
        const int ms = tableOf(strNumOf(p, "maskSize", "3"), kDistMask, sizeof(kDistMask) / sizeof(Code), cv::DIST_MASK_3);
        if (boolOf(p, "labels", false)) {
            Mat dist, labels;
            cv::distanceTransform(g, dist, labels, dt, ms,
                                  tableOf(strNumOf(p, "labelType", "ccomp"), kDistLabel, sizeof(kDistLabel) / sizeof(Code), cv::DIST_LABEL_CCOMP));
            return jOkMats({ put(std::move(dist)), put(std::move(labels)) });
        }
        Mat dist; cv::distanceTransform(g, dist, dt, ms);
        return jOkMats({ put(std::move(dist)) });
    };
    ops["floodFill"] = [M, store, this](const Json::Value& p) {
        Mat dst = M(p, "src").clone();      // floodFill 就地改图
        Mat mask;                           // (rows+2)x(cols+2) 的 8U: 填过的点 = 1, 边界 = 2
        if (p.isMember("mask") && p["mask"].isNumeric()) {
            need(p["mask"].asInt()).convertTo(mask, CV_8UC1);   // 拷一份(别改调用方的图); 非 0 处会挡住填充
            if (mask.rows != dst.rows + 2 || mask.cols != dst.cols + 2) mask = Mat();
        }
        Rect rect;
        const int n = cv::floodFill(dst, mask, pointOf(p, "seedPoint"),
                                    scalarOf(p, "newVal", Scalar(255, 255, 255)), &rect,
                                    scalarOf(p, "loDiff", Scalar()), scalarOf(p, "upDiff", Scalar()),
                                    intOf(p, "flags", 4));
        Json::Value v = jOkMat(put(std::move(dst)));
        v["value"] = n;                     // 填充了多少像素(OpenCV 的返回值)
        v["mask"] = mask.empty() ? 0 : put(std::move(mask));
        Json::Value r; r["x"] = rect.x; r["y"] = rect.y; r["w"] = rect.width; r["h"] = rect.height;
        v["rect"] = r;
        return v;
    };
    ops["watershed"] = [M, store](const Json::Value& p) {
        Mat markers = M(p, "markers").clone();      // watershed 就地改 markers(而且要求 CV_32S)
        if (markers.type() != CV_32SC1) markers.convertTo(markers, CV_32SC1);
        const Mat& img = M(p, "src");
        Mat bgr;                                    // watershed 要 CV_8UC3
        if (img.channels() == 1) {
            Mat g8; toGray(img).convertTo(g8, CV_8UC1);
            cv::cvtColor(g8, bgr, cv::COLOR_GRAY2BGR);
        } else if (img.type() == CV_8UC3) {
            bgr = img;
        } else {
            img.convertTo(bgr, CV_8UC3);
        }
        cv::watershed(bgr, markers);
        return store(std::move(markers));
    };
    ops["kmeans"] = [M, store, tableOf, termOf, this](const Json::Value& p) {
        Mat f32; M(p, "data").convertTo(f32, CV_32F);
        if (f32.channels() > 1) f32 = f32.reshape(1, f32.rows);      // 每行一个样本
        Mat labels, centers;
        const double compact = cv::kmeans(f32, intOf(p, "k", 2), labels, termOf(p), intOf(p, "attempts", 3),
                                          tableOf(strNumOf(p, "flags", "pp"), kKmeans, sizeof(kKmeans) / sizeof(Code), cv::KMEANS_PP_CENTERS),
                                          centers);
        Json::Value v = jOkMats({ put(std::move(labels)), put(std::move(centers)) });
        v["value"] = compact;               // kmeans 的紧凑度(越小越紧)
        return v;
    };
    ops["cornerHarris"] = [M, store](const Json::Value& p) {
        Mat d;
        cv::cornerHarris(toGray(M(p, "src")), d, intOf(p, "blockSize", 2), intOf(p, "ksize", 3),
                         numOf(p, "k", 0.04), borderOf(strNumOf(p, "borderType", "default")));
        return store(std::move(d));         // CV_32F 响应图(自己再阈值/取极大)
    };
    ops["goodFeaturesToTrack"] = [M, store](const Json::Value& p) {
        Mat d;
        cv::goodFeaturesToTrack(toGray(M(p, "src")), d, intOf(p, "maxCorners", 100),
                                numOf(p, "qualityLevel", 0.01), numOf(p, "minDistance", 10), cv::noArray(),
                                intOf(p, "blockSize", 3), boolOf(p, "useHarrisDetector", false), numOf(p, "k", 0.04));
        return store(std::move(d));         // Nx1 CV_32FC2 (x, y)
    };

    // —— (5) 轮廓 / 形状(给 findContours 的结果直接用) ——
    ops["drawContours"] = [M, store, ptsOf, tableOf, this](const Json::Value& p) {
        if (!p.isMember("contours") || !p["contours"].isArray()) throw std::runtime_error("drawContours 要传 contours: [轮廓Mat 或 [[x,y],...], ...]");
        std::vector<std::vector<Point>> cs;
        for (const auto& one : p["contours"]) {
            std::vector<Point> v;
            if (one.isNumeric()) v = matToPoints(need(one.asInt()));
            else for (const Point2f& q : ptsOf(one)) v.push_back(Point(cvRound(q.x), cvRound(q.y)));
            if (!v.empty()) cs.push_back(v);
        }
        Mat dst = M(p, "src").clone();
        cv::drawContours(dst, cs, intOf(p, "contourIdx", -1), scalarOf(p, "color", Scalar(0, 255, 0)),
                         intOf(p, "thickness", 1), lineTypeOf(p));
        return store(std::move(dst));
    };
    ops["minEnclosingCircle"] = [ptsF](const Json::Value& p) {
        const std::vector<Point2f> pts = ptsF(p);
        if (pts.empty()) throw std::runtime_error("minEnclosingCircle 要传 pts 或 contour");
        Point2f c; float r = 0;
        cv::minEnclosingCircle(pts, c, r);
        Json::Value v = jOk(), ctr(Json::arrayValue);
        ctr.append(c.x); ctr.append(c.y);
        v["center"] = ctr; v["radius"] = r;
        return v;
    };
    ops["fitEllipse"] = [ptsF](const Json::Value& p) {
        const std::vector<Point2f> pts = ptsF(p);
        if (pts.size() < 5) throw std::runtime_error("fitEllipse 至少要 5 个点");
        const cv::RotatedRect rr = cv::fitEllipse(pts);
        Json::Value v = jOk(), c(Json::arrayValue), s(Json::arrayValue);
        c.append(rr.center.x); c.append(rr.center.y);
        s.append(rr.size.width); s.append(rr.size.height);
        v["center"] = c; v["size"] = s; v["angle"] = rr.angle;
        return v;
    };
    ops["fitLine"] = [ptsF, store, tableOf](const Json::Value& p) {
        const std::vector<Point2f> pts = ptsF(p);
        if (pts.size() < 2) throw std::runtime_error("fitLine 至少要 2 个点");
        Mat line;
        cv::fitLine(pts, line, tableOf(strNumOf(p, "distType", "l2"), kFitLine, sizeof(kFitLine) / sizeof(Code), cv::DIST_L2),
                    numOf(p, "param", 0), numOf(p, "reps", 0.01), numOf(p, "aeps", 0.01));
        return store(std::move(line));      // 4x1 CV_32F: (vx, vy, x0, y0)
    };
    ops["matchShapes"] = [ptsOf, tableOf](const Json::Value& p) {
        const std::vector<Point2f> a = ptsOf(p["a"]), b = ptsOf(p["b"]);
        if (a.size() < 3 || b.size() < 3) throw std::runtime_error("matchShapes 要两组点(每组 >=3)");
        Json::Value v = jOk();
        v["value"] = cv::matchShapes(a, b,
            tableOf(strNumOf(p, "method", "i1"), kShapeMatch, sizeof(kShapeMatch) / sizeof(Code), cv::CONTOURS_MATCH_I1),
            numOf(p, "parameter", 0));
        return v;
    };
    ops["convexityDefects"] = [ptsF, this, store](const Json::Value& p) {
        const std::vector<Point2f> pts = ptsF(p);
        if (pts.size() < 4) throw std::runtime_error("convexityDefects 至少要 4 个点");
        std::vector<int> hullIdx;
        if (p.isMember("hull") && p["hull"].isNumeric()) {       // OpenCV.js 会自己传 hull(下标数组)
            const Mat hm = need(p["hull"].asInt());
            if (hm.type() != CV_32SC1) throw std::runtime_error("hull 要是 CV_32SC1 的凸包下标数组");
            const int* d = (const int*)hm.data;
            for (int i = 0; i < hm.rows * hm.cols; ++i) hullIdx.push_back(d[i]);
        } else {
            cv::convexHull(pts, hullIdx, true, false);           // returnPoints=false -> 下标
        }
        // ⚠ convexityDefects 要的"轮廓"必须是 2 通道 Mat(传 vector<Point2f> 会踩 npoints>=0 断言)
        std::vector<Point> ipts;
        for (const Point2f& q : pts) ipts.push_back(Point(cvRound(q.x), cvRound(q.y)));
        Mat contourMat = pointsToMat(ipts);
        Mat defects, hullMat = indicesToMat(hullIdx);
        cv::convexityDefects(contourMat, hullMat, defects);
        return store(std::move(defects));   // Nx1 CV_32SC4: (起点idx, 终点idx, 最远点idx, 深度*256)
    };

    // —— (6) 直方图比对 / 反投影 ——
    ops["compareHist"] = [M, tableOf](const Json::Value& p) {
        Mat a = M(p, "a"), b = M(p, "b");       // 浅拷贝: 只可能在类型不同时**各自**重新分配
        if (a.depth() != CV_32F) a.convertTo(a, CV_32F);
        if (b.depth() != CV_32F) b.convertTo(b, CV_32F);
        Json::Value v = jOk();
        v["value"] = cv::compareHist(a, b,
            tableOf(strNumOf(p, "method", "correl"), kHistCmp, sizeof(kHistCmp) / sizeof(Code), cv::HISTCMP_CORREL));
        return v;
    };
    ops["calcBackProject"] = [M, store](const Json::Value& p) {
        const Mat src = M(p, "src"), hist = M(p, "hist");
        int ch = intOf(p, "channel", 0);
        float range[] = { (float)numOf(p, "lo", 0), (float)numOf(p, "hi", 256) };
        const float* ranges[] = { range };
        Mat dst;
        cv::calcBackProject(&src, 1, &ch, hist, dst, ranges, numOf(p, "scale", 1));
        return store(std::move(dst));
    };

    // —— (7) 几何 / 去噪 / 光流 ——
    ops["findHomography"] = [ptsOf, store, tableOf, this](const Json::Value& p) {
        const std::vector<Point2f> s = ptsOf(p["a"]), d = ptsOf(p["b"]);
        if (s.size() < 4 || s.size() != d.size()) throw std::runtime_error("findHomography 要 >=4 对点, 且两组等长");
        Mat mask;
        Mat h = cv::findHomography(s, d,
            tableOf(strNumOf(p, "method", "ransac"), kHomography, sizeof(kHomography) / sizeof(Code), cv::RANSAC),
            numOf(p, "ransacReprojThreshold", 3), mask, intOf(p, "maxIters", 2000), numOf(p, "confidence", 0.995));
        Json::Value v = jOkMat(put(std::move(h)));
        v["mask"] = mask.empty() ? 0 : put(std::move(mask));   // 内点掩膜(RANSAC/LMEDS/RHO 才有)
        return v;
    };
    ops["undistort"] = [M, store](const Json::Value& p) {
        Mat d;
        if (p.isMember("distCoeffs")) d = M(p, "distCoeffs");
        Mat dst;
        if (p.isMember("newCameraMatrix")) cv::undistort(M(p, "src"), dst, M(p, "cameraMatrix"), d, M(p, "newCameraMatrix"));
        else cv::undistort(M(p, "src"), dst, M(p, "cameraMatrix"), d);
        return store(std::move(dst));
    };
    ops["remap"] = [M, store](const Json::Value& p) {
        Mat dst;
        cv::remap(M(p, "src"), dst, M(p, "map1"), p.isMember("map2") ? M(p, "map2") : Mat(),
                  interpOf(strNumOf(p, "interpolation", "linear")),
                  borderOf(strNumOf(p, "borderMode", "constant")),
                  scalarOf(p, "borderValue", Scalar()));
        return store(std::move(dst));
    };
    ops["fastNlMeansDenoising"] = [](const Json::Value&) -> Json::Value {
        throw std::runtime_error("fastNlMeansDenoising 在 photo 模块里, 而本机的 OpenCV 构建是 "
                                 "BUILD_opencv_photo=OFF —— 这个能力没有; 去噪请用 medianBlur / bilateralFilter");
    };
    ops["inpaint"] = [](const Json::Value&) -> Json::Value {
        throw std::runtime_error("inpaint 在 photo 模块里, 而本机的 OpenCV 构建是 BUILD_opencv_photo=OFF; "
                                 "小面积修复可以自己用 inRange + morphologyEx + 邻域均值凑");
    };
    ops["calcOpticalFlowFarneback"] = [M, store](const Json::Value& p) {
        const Mat prev = toGray(M(p, "prev")), next = toGray(M(p, "next"));
        Mat flow;
        cv::calcOpticalFlowFarneback(prev, next, flow, numOf(p, "pyrScale", 0.5), intOf(p, "levels", 3),
                                     intOf(p, "winsize", 15), intOf(p, "iterations", 3), intOf(p, "polyN", 5),
                                     numOf(p, "polySigma", 1.2), intOf(p, "flags", 0));
        return store(std::move(flow));      // 与输入同尺寸的 CV_32FC2: (dx, dy)
    };
}

// ============================================================================
// 桥接实现(编入 OpenCV 的版本)
// ============================================================================
bool JZJSCvModule::jsAvailable() { return true; }

int JZJSCvModule::jsNew(const std::string& optsJson) {
    Json::Value p; Json::Reader reader;
    if (!optsJson.empty() && !reader.parse(optsJson, p)) return 0;
    try {
        const int type = typeOf(strNumOf(p, "type", "8UC3"));
        const int cols = intOf(p, "cols", 0), rows = intOf(p, "rows", 0);
        if (cols <= 0 || rows <= 0) return 0;
        const std::string kind = lower(strOf(p, "kind", "zeros"));
        Mat m;
        if (kind == "ones") m = Mat::ones(rows, cols, type);
        else if (kind == "empty" || kind == "uninit") m = Mat(rows, cols, type);
        else if (p.isMember("fill")) m = Mat(rows, cols, type, scalarOf(p, "fill"));
        else m = Mat::zeros(rows, cols, type);
        return m_impl->put(std::move(m));
    } catch (const std::exception&) { return 0; }
}

Json::Value JZJSCvModule::jsInfo(int id) {
    const Mat* m = m_impl->find(id);
    if (!m) { Json::Value v; v["error"] = "无效的图像句柄 " + std::to_string(id); return v; }
    Json::Value v;
    v["cols"] = m->cols; v["rows"] = m->rows;
    v["channels"] = m->channels(); v["depth"] = m->depth();
    v["type"] = m->type(); v["typeName"] = typeNameOf(m->type());
    v["empty"] = m->empty(); v["total"] = (Json::Int64)m->total();
    // ⚠ 空图(比如"没找到圆"的 HoughCircles / "没有凸缺陷"的 convexityDefects)是**正常结果**:
    //   OpenCV.js 侧 `mat.rows/cols` 给 0, 而 `Mat::elemSize()` 对空图会直接断言(res != 0) —— 这里兜住,
    //   否则空结果一读 rows 就 InternalError(实测踩到)。
    v["elemSize"] = m->empty() ? 0 : (int)m->elemSize();
    v["continuous"] = m->empty() ? true : m->isContinuous();
    return v;
}

int JZJSCvModule::jsClone(int id) {
    const Mat* m = m_impl->find(id);
    return m ? m_impl->put(m->clone()) : 0;
}

bool JZJSCvModule::jsRelease(int id) { return m_impl->mats.erase(id) > 0; }

int JZJSCvModule::jsReleaseAll() {
    const int n = (int)m_impl->mats.size();
    m_impl->mats.clear();
    return n;
}

int JZJSCvModule::jsCount() { return (int)m_impl->mats.size(); }

int JZJSCvModule::jsImread(const std::string& path) {
    Mat m = cv::imread(path, cv::IMREAD_UNCHANGED);      // 先按原样读: 该不该丢 alpha 由脚本决定
    if (m.empty()) m = cv::imread(path, cv::IMREAD_COLOR);
    if (m.empty()) return 0;
    return m_impl->put(std::move(m));
}

bool JZJSCvModule::jsImwrite(int id, const std::string& path) {
    const Mat* m = m_impl->find(id);
    if (!m || m->empty()) return false;
    try { return cv::imwrite(path, *m); } catch (const std::exception&) { return false; }
}

int JZJSCvModule::jsImdecode(const std::string& data) {
    std::vector<unsigned char> buf;
    if (!b64Decode(data, buf)) return 0;
    Mat m = cv::imdecode(buf, cv::IMREAD_UNCHANGED);
    if (m.empty()) m = cv::imdecode(buf, cv::IMREAD_COLOR);
    return m.empty() ? 0 : m_impl->put(std::move(m));
}

std::string JZJSCvModule::jsImencode(int id, const std::string& ext) {
    const Mat* m = m_impl->find(id);
    if (!m || m->empty()) return std::string();
    std::string e = ext.empty() ? ".png" : ext;
    if (e[0] != '.') e = "." + e;
    std::vector<unsigned char> buf;
    try { if (!cv::imencode(e, *m, buf)) return std::string(); } catch (const std::exception&) { return std::string(); }
    const std::string mime = (e == ".jpg" || e == ".jpeg") ? "image/jpeg"
                           : (e == ".bmp") ? "image/bmp"
                           : (e == ".webp") ? "image/webp" : "image/png";
    return "data:" + mime + ";base64," + b64Encode(buf.data(), buf.size());
}

std::string JZJSCvModule::jsData(int id) {
    const Mat* m = m_impl->find(id);
    if (!m || m->empty()) return std::string();
    Mat t;
    (*m).copyTo(t);                                  // 单独一份(下面可能要补连续)
    // ⚠ 不做类型转换: 给的就是**原始字节**(32F 就是 4 字节/通道的 float 位模式), 与 OpenCV.js 的
    //   mat.data / data32F 语义一致; 调用方拿着 info() 里的 type 自己解释。以前这里会把非 8U 转成
    //   8U, 于是 data32F 只能报错。
    if (!t.isContinuous()) t = t.clone();
    return b64Encode(t.data, t.total() * t.elemSize());
}

int JZJSCvModule::jsFromData(const std::string& dataB64, const std::string& optsJson) {
    Json::Value p; Json::Reader reader;
    if (!optsJson.empty() && !reader.parse(optsJson, p)) return 0;
    std::vector<unsigned char> buf;
    if (!b64Decode(dataB64, buf)) return 0;
    try {
        const int type = typeOf(strNumOf(p, "type", "8UC1"));
        const int cols = intOf(p, "cols", 0), rows = intOf(p, "rows", 0);
        if (cols <= 0 || rows <= 0) return 0;
        const size_t need = (size_t)cols * rows * CV_ELEM_SIZE(type);
        if (buf.size() < need) return 0;
        Mat m(rows, cols, type);
        memcpy(m.data, buf.data(), need);
        return m_impl->put(std::move(m));
    } catch (const std::exception&) { return 0; }
}

Json::Value JZJSCvModule::jsPixel(int id, int x, int y) {
    const Mat* m = m_impl->find(id);
    if (!m || x < 0 || y < 0 || x >= m->cols || y >= m->rows) return Json::Value();
    const int cn = m->channels();
    const uchar* row = m->ptr<uchar>(y) + (size_t)x * m->elemSize();   // 按 elemSize 走, 不越界
    Json::Value out(Json::arrayValue);
    switch (m->depth()) {
    case CV_8U:  for (int c = 0; c < cn; ++c) out.append((int)((const uchar*)row)[c]); break;
    case CV_8S:  for (int c = 0; c < cn; ++c) out.append((int)((const schar*)row)[c]); break;
    case CV_16U: for (int c = 0; c < cn; ++c) out.append((int)((const ushort*)row)[c]); break;
    case CV_16S: for (int c = 0; c < cn; ++c) out.append((int)((const short*)row)[c]); break;
    case CV_32S: for (int c = 0; c < cn; ++c) out.append(((const int*)row)[c]); break;
    case CV_32F: for (int c = 0; c < cn; ++c) out.append((double)((const float*)row)[c]); break;
    case CV_64F: for (int c = 0; c < cn; ++c) out.append(((const double*)row)[c]); break;
    default: return Json::Value();
    }
    return out;
}

bool JZJSCvModule::jsSetPixel(int id, int x, int y, const std::string& valuesJson) {
    Mat* m = m_impl->find(id);
    if (!m || x < 0 || y < 0 || x >= m->cols || y >= m->rows) return false;
    Json::Value v; Json::Reader reader;
    if (!reader.parse(valuesJson, v) || !v.isArray() || v.size() < 1) return false;
    const int cn = std::min((int)v.size(), m->channels());
    uchar* row = m->ptr<uchar>(y) + (size_t)x * m->elemSize();
    switch (m->depth()) {
    case CV_8U:  for (int c = 0; c < cn; ++c) ((uchar*)row)[c] = (uchar)v[(Json::Value::ArrayIndex)c].asInt(); break;
    case CV_8S:  for (int c = 0; c < cn; ++c) ((schar*)row)[c] = (schar)v[(Json::Value::ArrayIndex)c].asInt(); break;
    case CV_16U: for (int c = 0; c < cn; ++c) ((ushort*)row)[c] = (ushort)v[(Json::Value::ArrayIndex)c].asInt(); break;
    case CV_16S: for (int c = 0; c < cn; ++c) ((short*)row)[c] = (short)v[(Json::Value::ArrayIndex)c].asInt(); break;
    case CV_32S: for (int c = 0; c < cn; ++c) ((int*)row)[c] = v[(Json::Value::ArrayIndex)c].asInt(); break;
    case CV_32F: for (int c = 0; c < cn; ++c) ((float*)row)[c] = (float)v[(Json::Value::ArrayIndex)c].asDouble(); break;
    case CV_64F: for (int c = 0; c < cn; ++c) ((double*)row)[c] = v[(Json::Value::ArrayIndex)c].asDouble(); break;
    default: return false;
    }
    return true;
}

Json::Value JZJSCvModule::jsOp(const std::string& name, const std::string& paramsJson) {
    if (!m_impl) return jFail("模块未初始化");
    m_impl->buildOps();
    auto it = m_impl->ops.find(name);
    if (it == m_impl->ops.end()) {
        std::string all;
        for (const auto& kv : m_impl->ops) { if (!all.empty()) all += ", "; all += kv.first; }
        return jFail("不支持的操作 '" + name + "'（jsOps() 可列全部；现有: " + all + "）");
    }
    Json::Value p;
    if (!paramsJson.empty()) {
        Json::Reader reader;
        if (!reader.parse(paramsJson, p)) return jFail("参数不是合法 JSON");
    }
    if (!p.isObject()) p = Json::Value(Json::objectValue);
    try {
        return it->second(p);
    } catch (const std::exception& e) {
        return jFail(e.what());
    }
}

Json::Value JZJSCvModule::jsOps() {
    m_impl->buildOps();
    Json::Value a(Json::arrayValue);
    for (const auto& kv : m_impl->ops) a.append(kv.first);
    return a;
}

#else
// ============================================================================
// 没编 OpenCV 的版本: 一律报"未编入", 绝不假装成功
// ============================================================================
struct JZJSCvModule::Impl {};

namespace {
const char* kNoCv = "本构建未编入 OpenCV(JZ_CU_OPENCV): __jz.cv 不可用";
Json::Value jFail(const std::string& msg) { Json::Value v; v["ok"] = false; v["error"] = msg; return v; }
} // namespace

bool JZJSCvModule::jsAvailable() { return false; }
int JZJSCvModule::jsNew(const std::string&) { return 0; }
Json::Value JZJSCvModule::jsInfo(int) { Json::Value v; v["error"] = kNoCv; return v; }
int JZJSCvModule::jsClone(int) { return 0; }
bool JZJSCvModule::jsRelease(int) { return false; }
int JZJSCvModule::jsReleaseAll() { return 0; }
int JZJSCvModule::jsCount() { return 0; }
int JZJSCvModule::jsImread(const std::string&) { return 0; }
bool JZJSCvModule::jsImwrite(int, const std::string&) { return false; }
int JZJSCvModule::jsImdecode(const std::string&) { return 0; }
std::string JZJSCvModule::jsImencode(int, const std::string&) { return std::string(); }
std::string JZJSCvModule::jsData(int) { return std::string(); }
int JZJSCvModule::jsFromData(const std::string&, const std::string&) { return 0; }
Json::Value JZJSCvModule::jsPixel(int, int, int) { return Json::Value(); }
bool JZJSCvModule::jsSetPixel(int, int, int, const std::string&) { return false; }
Json::Value JZJSCvModule::jsOp(const std::string&, const std::string&) { return jFail(kNoCv); }
Json::Value JZJSCvModule::jsOps() { return Json::Value(Json::arrayValue); }

#endif

// ============================================================================
// 注册(两版共用): __jz.cv.*
// ============================================================================
void JZJSCvModule::initEngine(JZJSEngine* engine) {
    m_engine = engine;
    if (!m_impl) m_impl.reset(new Impl());
    JSValue global = m_engine->globalObject();

    // 复用已存在的 __jz(不要覆盖 net/core 建好的子命名空间; 与 JZJSComputerUseModule 同款)
    JSValue jz = m_engine->getProperty(global, "__jz");
    const bool fromGet = !JS_IsUndefined(jz);
    if (!fromGet) {
        jz = m_engine->createObject();
        m_engine->setProperty(global, "__jz", jz);
    }

    JSValue ns = m_engine->createObject();
    // 能力探测: 脚本/适配层先问一句"这个 exe 能不能做图像处理", 再决定走哪条路
    m_engine->registFunction(ns, "available",  jsbind::createSelfFunction(this, &JZJSCvModule::jsAvailable));
    // 句柄 / 生命周期
    m_engine->registFunction(ns, "create",     jsbind::createSelfFunction(this, &JZJSCvModule::jsNew));
    m_engine->registFunction(ns, "info",       jsbind::createSelfFunction(this, &JZJSCvModule::jsInfo));
    m_engine->registFunction(ns, "clone",      jsbind::createSelfFunction(this, &JZJSCvModule::jsClone));
    m_engine->registFunction(ns, "release",    jsbind::createSelfFunction(this, &JZJSCvModule::jsRelease));
    m_engine->registFunction(ns, "releaseAll", jsbind::createSelfFunction(this, &JZJSCvModule::jsReleaseAll));
    m_engine->registFunction(ns, "count",      jsbind::createSelfFunction(this, &JZJSCvModule::jsCount));
    // 磁盘 / 编解码
    m_engine->registFunction(ns, "imread",     jsbind::createSelfFunction(this, &JZJSCvModule::jsImread));
    m_engine->registFunction(ns, "imwrite",    jsbind::createSelfFunction(this, &JZJSCvModule::jsImwrite));
    m_engine->registFunction(ns, "imdecode",   jsbind::createSelfFunction(this, &JZJSCvModule::jsImdecode));
    m_engine->registFunction(ns, "imencode",   jsbind::createSelfFunction(this, &JZJSCvModule::jsImencode));
    m_engine->registFunction(ns, "data",       jsbind::createSelfFunction(this, &JZJSCvModule::jsData));
    m_engine->registFunction(ns, "fromData",   jsbind::createSelfFunction(this, &JZJSCvModule::jsFromData));
    // 逐像素
    m_engine->registFunction(ns, "pixel",      jsbind::createSelfFunction(this, &JZJSCvModule::jsPixel));
    m_engine->registFunction(ns, "setPixel",   jsbind::createSelfFunction(this, &JZJSCvModule::jsSetPixel));
    // 通用运算(颜色/阈值/滤波/形态学/几何/轮廓/统计/绘制 都在这里)
    m_engine->registFunction(ns, "op",         jsbind::createSelfFunction(this, &JZJSCvModule::jsOp));
    m_engine->registFunction(ns, "ops",        jsbind::createSelfFunction(this, &JZJSCvModule::jsOps));
    m_engine->setProperty(jz, "cv", ns);

    // 释放 getProperty 多 dup 的引用, 避免 engine 析构时 gc_obj_list 非空断言
    if (fromGet) JS_FreeValue(m_engine->context(), jz);
}

void JZJSCvModule::deinitEngine() {
    // 直接析构 Impl: 连同句柄表一起没了 ⇒ 挂着的 cv::Mat(可能几十 MB 一张图)全部释放。
    // ⚠ 不要写成 m_impl->mats.clear(): 那是"编入 OpenCV"版才有的成员, 退化版没有。
    m_impl.reset();
    m_engine = nullptr;
}

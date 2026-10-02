// cv 常量表 —— OpenCV.js 的 cv.XXX 在这里**同名同值**。
//
// 数值不是手抄的: 2026-10-02 从真 opencv-js(@techstark/opencv-js 5.0.0-release.1)导出, 再用本机
// OpenCV **4.10 头文件**逐个核对过(274 个名字零差异), 所以跟 OpenCV.js 教程/示例里的
// cv.COLOR_BGR2GRAY / cv.THRESH_BINARY + cv.THRESH_OTSU 完全是同一个数。
//
// ⚠ 唯一的例外是 CV_* 一族: OpenCV 5 把通道位数从 3 位改成 5 位(8UC3 = 64), 而我们是 4.x
//   (8UC3 = **16**), 照抄 5.x 会让 cv::Mat 算成 9 通道。这一族按 4.x 的
//   CV_MAKETYPE(depth,cn) = depth + ((cn-1) << 3) 生成。
//
// 为什么可以只给数字: C++ 侧码表参数(colorCodeOf / threshOf / morphOf / retrOf / chainOf /
// interpOf / borderOf / tmOf / normOf / cmpOf / fontOf / colormapOf)都加了"整型直通"
// (JZJSCvModule.cpp 的 numericKey), 数字原样交给 OpenCV —— 于是**官方有而我们名字表里没有**
// 的那些码(如 COLOR_BGR2Luv / BORDER_TRANSPARENT / TM_SQDIFF)也照样能用。
//
// ⚠ 本文件由 gen_consts.py + consts.json 生成; 改值请以 OpenCV 官方 header 为准。
/* eslint-disable */

// ---- CV_ (OpenCV 4.x 的通道编码: CV_MAKETYPE(depth,cn) = depth + ((cn-1) << 3)) ----
export const CV_8U = 0;
export const CV_8S = 1;
export const CV_16U = 2;
export const CV_16S = 3;
export const CV_32S = 4;
export const CV_32F = 5;
export const CV_64F = 6;
export const CV_8UC1 = 0;
export const CV_8UC2 = 8;
export const CV_8UC3 = 16;
export const CV_8UC4 = 24;
export const CV_8SC1 = 1;
export const CV_8SC2 = 9;
export const CV_8SC3 = 17;
export const CV_8SC4 = 25;
export const CV_16UC1 = 2;
export const CV_16UC2 = 10;
export const CV_16UC3 = 18;
export const CV_16UC4 = 26;
export const CV_16SC1 = 3;
export const CV_16SC2 = 11;
export const CV_16SC3 = 19;
export const CV_16SC4 = 27;
export const CV_32SC1 = 4;
export const CV_32SC2 = 12;
export const CV_32SC3 = 20;
export const CV_32SC4 = 28;
export const CV_32FC1 = 5;
export const CV_32FC2 = 13;
export const CV_32FC3 = 21;
export const CV_32FC4 = 29;
export const CV_64FC1 = 6;
export const CV_64FC2 = 14;
export const CV_64FC3 = 22;
export const CV_64FC4 = 30;
export const CV_16F = 7;

// ---- COLOR ----
export const COLOR_BGR2BGR555 = 22;
export const COLOR_BGR2BGR565 = 12;
export const COLOR_BGR2BGRA = 0;
export const COLOR_BGR2GRAY = 6;
export const COLOR_BGR2HLS = 52;
export const COLOR_BGR2HLS_FULL = 68;
export const COLOR_BGR2HSV = 40;
export const COLOR_BGR2HSV_FULL = 66;
export const COLOR_BGR2Lab = 44;
export const COLOR_BGR2Luv = 50;
export const COLOR_BGR2RGB = 4;
export const COLOR_BGR2RGBA = 2;
export const COLOR_BGR2XYZ = 32;
export const COLOR_BGR2YCrCb = 36;
export const COLOR_BGR2YUV = 82;
export const COLOR_BGR2YUV_I420 = 128;
export const COLOR_BGR2YUV_IYUV = 128;
export const COLOR_BGR2YUV_UYNV = 144;
export const COLOR_BGR2YUV_UYVY = 144;
export const COLOR_BGR2YUV_Y422 = 144;
export const COLOR_BGR2YUV_YUNV = 148;
export const COLOR_BGR2YUV_YUY2 = 148;
export const COLOR_BGR2YUV_YUYV = 148;
export const COLOR_BGR2YUV_YV12 = 132;
export const COLOR_BGR2YUV_YVYU = 150;
export const COLOR_BGR5552BGR = 24;
export const COLOR_BGR5552BGRA = 28;
export const COLOR_BGR5552GRAY = 31;
export const COLOR_BGR5552RGB = 25;
export const COLOR_BGR5552RGBA = 29;
export const COLOR_BGR5652BGR = 14;
export const COLOR_BGR5652BGRA = 18;
export const COLOR_BGR5652GRAY = 21;
export const COLOR_BGR5652RGB = 15;
export const COLOR_BGR5652RGBA = 19;
export const COLOR_BGRA2BGR = 1;
export const COLOR_BGRA2BGR555 = 26;
export const COLOR_BGRA2BGR565 = 16;
export const COLOR_BGRA2GRAY = 10;
export const COLOR_BGRA2RGB = 3;
export const COLOR_BGRA2RGBA = 5;
export const COLOR_BGRA2YUV_I420 = 130;
export const COLOR_BGRA2YUV_IYUV = 130;
export const COLOR_BGRA2YUV_UYNV = 146;
export const COLOR_BGRA2YUV_UYVY = 146;
export const COLOR_BGRA2YUV_Y422 = 146;
export const COLOR_BGRA2YUV_YUNV = 152;
export const COLOR_BGRA2YUV_YUY2 = 152;
export const COLOR_BGRA2YUV_YUYV = 152;
export const COLOR_BGRA2YUV_YV12 = 134;
export const COLOR_BGRA2YUV_YVYU = 154;
export const COLOR_BayerBG2BGR = 46;
export const COLOR_BayerBG2BGRA = 139;
export const COLOR_BayerBG2BGR_EA = 135;
export const COLOR_BayerBG2BGR_VNG = 62;
export const COLOR_BayerBG2GRAY = 86;
export const COLOR_BayerBG2RGB = 48;
export const COLOR_BayerBG2RGBA = 141;
export const COLOR_BayerBG2RGB_EA = 137;
export const COLOR_BayerBG2RGB_VNG = 64;
export const COLOR_BayerBGGR2BGR = 48;
export const COLOR_BayerBGGR2BGRA = 141;
export const COLOR_BayerBGGR2BGR_EA = 137;
export const COLOR_BayerBGGR2BGR_VNG = 64;
export const COLOR_BayerBGGR2GRAY = 88;
export const COLOR_BayerBGGR2RGB = 46;
export const COLOR_BayerBGGR2RGBA = 139;
export const COLOR_BayerBGGR2RGB_EA = 135;
export const COLOR_BayerBGGR2RGB_VNG = 62;
export const COLOR_BayerGB2BGR = 47;
export const COLOR_BayerGB2BGRA = 140;
export const COLOR_BayerGB2BGR_EA = 136;
export const COLOR_BayerGB2BGR_VNG = 63;
export const COLOR_BayerGB2GRAY = 87;
export const COLOR_BayerGB2RGB = 49;
export const COLOR_BayerGB2RGBA = 142;
export const COLOR_BayerGB2RGB_EA = 138;
export const COLOR_BayerGB2RGB_VNG = 65;
export const COLOR_BayerGBRG2BGR = 49;
export const COLOR_BayerGBRG2BGRA = 142;
export const COLOR_BayerGBRG2BGR_EA = 138;
export const COLOR_BayerGBRG2BGR_VNG = 65;
export const COLOR_BayerGBRG2GRAY = 89;
export const COLOR_BayerGBRG2RGB = 47;
export const COLOR_BayerGBRG2RGBA = 140;
export const COLOR_BayerGBRG2RGB_EA = 136;
export const COLOR_BayerGBRG2RGB_VNG = 63;
export const COLOR_BayerGR2BGR = 49;
export const COLOR_BayerGR2BGRA = 142;
export const COLOR_BayerGR2BGR_EA = 138;
export const COLOR_BayerGR2BGR_VNG = 65;
export const COLOR_BayerGR2GRAY = 89;
export const COLOR_BayerGR2RGB = 47;
export const COLOR_BayerGR2RGBA = 140;
export const COLOR_BayerGR2RGB_EA = 136;
export const COLOR_BayerGR2RGB_VNG = 63;
export const COLOR_BayerGRBG2BGR = 47;
export const COLOR_BayerGRBG2BGRA = 140;
export const COLOR_BayerGRBG2BGR_EA = 136;
export const COLOR_BayerGRBG2BGR_VNG = 63;
export const COLOR_BayerGRBG2GRAY = 87;
export const COLOR_BayerGRBG2RGB = 49;
export const COLOR_BayerGRBG2RGBA = 142;
export const COLOR_BayerGRBG2RGB_EA = 138;
export const COLOR_BayerGRBG2RGB_VNG = 65;
export const COLOR_BayerRG2BGR = 48;
export const COLOR_BayerRG2BGRA = 141;
export const COLOR_BayerRG2BGR_EA = 137;
export const COLOR_BayerRG2BGR_VNG = 64;
export const COLOR_BayerRG2GRAY = 88;
export const COLOR_BayerRG2RGB = 46;
export const COLOR_BayerRG2RGBA = 139;
export const COLOR_BayerRG2RGB_EA = 135;
export const COLOR_BayerRG2RGB_VNG = 62;
export const COLOR_BayerRGGB2BGR = 46;
export const COLOR_BayerRGGB2BGRA = 139;
export const COLOR_BayerRGGB2BGR_EA = 135;
export const COLOR_BayerRGGB2BGR_VNG = 62;
export const COLOR_BayerRGGB2GRAY = 86;
export const COLOR_BayerRGGB2RGB = 48;
export const COLOR_BayerRGGB2RGBA = 141;
export const COLOR_BayerRGGB2RGB_EA = 137;
export const COLOR_BayerRGGB2RGB_VNG = 64;
export const COLOR_COLORCVT_MAX = 155;
export const COLOR_GRAY2BGR = 8;
export const COLOR_GRAY2BGR555 = 30;
export const COLOR_GRAY2BGR565 = 20;
export const COLOR_GRAY2BGRA = 9;
export const COLOR_GRAY2RGB = 8;
export const COLOR_GRAY2RGBA = 9;
export const COLOR_HLS2BGR = 60;
export const COLOR_HLS2BGR_FULL = 72;
export const COLOR_HLS2RGB = 61;
export const COLOR_HLS2RGB_FULL = 73;
export const COLOR_HSV2BGR = 54;
export const COLOR_HSV2BGR_FULL = 70;
export const COLOR_HSV2RGB = 55;
export const COLOR_HSV2RGB_FULL = 71;
export const COLOR_LBGR2Lab = 74;
export const COLOR_LBGR2Luv = 76;
export const COLOR_LRGB2Lab = 75;
export const COLOR_LRGB2Luv = 77;
export const COLOR_Lab2BGR = 56;
export const COLOR_Lab2LBGR = 78;
export const COLOR_Lab2LRGB = 79;
export const COLOR_Lab2RGB = 57;
export const COLOR_Luv2BGR = 58;
export const COLOR_Luv2LBGR = 80;
export const COLOR_Luv2LRGB = 81;
export const COLOR_Luv2RGB = 59;
export const COLOR_RGB2BGR = 4;
export const COLOR_RGB2BGR555 = 23;
export const COLOR_RGB2BGR565 = 13;
export const COLOR_RGB2BGRA = 2;
export const COLOR_RGB2GRAY = 7;
export const COLOR_RGB2HLS = 53;
export const COLOR_RGB2HLS_FULL = 69;
export const COLOR_RGB2HSV = 41;
export const COLOR_RGB2HSV_FULL = 67;
export const COLOR_RGB2Lab = 45;
export const COLOR_RGB2Luv = 51;
export const COLOR_RGB2RGBA = 0;
export const COLOR_RGB2XYZ = 33;
export const COLOR_RGB2YCrCb = 37;
export const COLOR_RGB2YUV = 83;
export const COLOR_RGB2YUV_I420 = 127;
export const COLOR_RGB2YUV_IYUV = 127;
export const COLOR_RGB2YUV_UYNV = 143;
export const COLOR_RGB2YUV_UYVY = 143;
export const COLOR_RGB2YUV_Y422 = 143;
export const COLOR_RGB2YUV_YUNV = 147;
export const COLOR_RGB2YUV_YUY2 = 147;
export const COLOR_RGB2YUV_YUYV = 147;
export const COLOR_RGB2YUV_YV12 = 131;
export const COLOR_RGB2YUV_YVYU = 149;
export const COLOR_RGBA2BGR = 3;
export const COLOR_RGBA2BGR555 = 27;
export const COLOR_RGBA2BGR565 = 17;
export const COLOR_RGBA2BGRA = 5;
export const COLOR_RGBA2GRAY = 11;
export const COLOR_RGBA2RGB = 1;
export const COLOR_RGBA2YUV_I420 = 129;
export const COLOR_RGBA2YUV_IYUV = 129;
export const COLOR_RGBA2YUV_UYNV = 145;
export const COLOR_RGBA2YUV_UYVY = 145;
export const COLOR_RGBA2YUV_Y422 = 145;
export const COLOR_RGBA2YUV_YUNV = 151;
export const COLOR_RGBA2YUV_YUY2 = 151;
export const COLOR_RGBA2YUV_YUYV = 151;
export const COLOR_RGBA2YUV_YV12 = 133;
export const COLOR_RGBA2YUV_YVYU = 153;
export const COLOR_RGBA2mRGBA = 125;
export const COLOR_SPACE_ADOBE_RGB = 2;
export const COLOR_SPACE_ADOBE_RGBL = 3;
export const COLOR_SPACE_APPLE_RGB = 10;
export const COLOR_SPACE_APPLE_RGBL = 11;
export const COLOR_SPACE_DCI_P3_RGB = 8;
export const COLOR_SPACE_DCI_P3_RGBL = 9;
export const COLOR_SPACE_LAB_A_10 = 33;
export const COLOR_SPACE_LAB_A_2 = 32;
export const COLOR_SPACE_LAB_D50_10 = 31;
export const COLOR_SPACE_LAB_D50_2 = 29;
export const COLOR_SPACE_LAB_D55_10 = 35;
export const COLOR_SPACE_LAB_D55_2 = 34;
export const COLOR_SPACE_LAB_D65_10 = 30;
export const COLOR_SPACE_LAB_D65_2 = 28;
export const COLOR_SPACE_LAB_D75_10 = 37;
export const COLOR_SPACE_LAB_D75_2 = 36;
export const COLOR_SPACE_LAB_E_10 = 39;
export const COLOR_SPACE_LAB_E_2 = 38;
export const COLOR_SPACE_PRO_PHOTO_RGB = 6;
export const COLOR_SPACE_PRO_PHOTO_RGBL = 7;
export const COLOR_SPACE_REC_2020_RGB = 14;
export const COLOR_SPACE_REC_2020_RGBL = 15;
export const COLOR_SPACE_REC_709_RGB = 12;
export const COLOR_SPACE_REC_709_RGBL = 13;
export const COLOR_SPACE_SRGB = 0;
export const COLOR_SPACE_SRGBL = 1;
export const COLOR_SPACE_WIDE_GAMUT_RGB = 4;
export const COLOR_SPACE_WIDE_GAMUT_RGBL = 5;
export const COLOR_SPACE_XYZ_A_10 = 21;
export const COLOR_SPACE_XYZ_A_2 = 20;
export const COLOR_SPACE_XYZ_D50_10 = 19;
export const COLOR_SPACE_XYZ_D50_2 = 17;
export const COLOR_SPACE_XYZ_D55_10 = 23;
export const COLOR_SPACE_XYZ_D55_2 = 22;
export const COLOR_SPACE_XYZ_D65_10 = 18;
export const COLOR_SPACE_XYZ_D65_2 = 16;
export const COLOR_SPACE_XYZ_D75_10 = 25;
export const COLOR_SPACE_XYZ_D75_2 = 24;
export const COLOR_SPACE_XYZ_E_10 = 27;
export const COLOR_SPACE_XYZ_E_2 = 26;
export const COLOR_XYZ2BGR = 34;
export const COLOR_XYZ2RGB = 35;
export const COLOR_YCrCb2BGR = 38;
export const COLOR_YCrCb2RGB = 39;
export const COLOR_YUV2BGR = 84;
export const COLOR_YUV2BGRA_I420 = 105;
export const COLOR_YUV2BGRA_IYUV = 105;
export const COLOR_YUV2BGRA_NV12 = 95;
export const COLOR_YUV2BGRA_NV21 = 97;
export const COLOR_YUV2BGRA_UYNV = 112;
export const COLOR_YUV2BGRA_UYVY = 112;
export const COLOR_YUV2BGRA_Y422 = 112;
export const COLOR_YUV2BGRA_YUNV = 120;
export const COLOR_YUV2BGRA_YUY2 = 120;
export const COLOR_YUV2BGRA_YUYV = 120;
export const COLOR_YUV2BGRA_YV12 = 103;
export const COLOR_YUV2BGRA_YVYU = 122;
export const COLOR_YUV2BGR_I420 = 101;
export const COLOR_YUV2BGR_IYUV = 101;
export const COLOR_YUV2BGR_NV12 = 91;
export const COLOR_YUV2BGR_NV21 = 93;
export const COLOR_YUV2BGR_UYNV = 108;
export const COLOR_YUV2BGR_UYVY = 108;
export const COLOR_YUV2BGR_Y422 = 108;
export const COLOR_YUV2BGR_YUNV = 116;
export const COLOR_YUV2BGR_YUY2 = 116;
export const COLOR_YUV2BGR_YUYV = 116;
export const COLOR_YUV2BGR_YV12 = 99;
export const COLOR_YUV2BGR_YVYU = 118;
export const COLOR_YUV2GRAY_420 = 106;
export const COLOR_YUV2GRAY_I420 = 106;
export const COLOR_YUV2GRAY_IYUV = 106;
export const COLOR_YUV2GRAY_NV12 = 106;
export const COLOR_YUV2GRAY_NV21 = 106;
export const COLOR_YUV2GRAY_UYNV = 123;
export const COLOR_YUV2GRAY_UYVY = 123;
export const COLOR_YUV2GRAY_Y422 = 123;
export const COLOR_YUV2GRAY_YUNV = 124;
export const COLOR_YUV2GRAY_YUY2 = 124;
export const COLOR_YUV2GRAY_YUYV = 124;
export const COLOR_YUV2GRAY_YV12 = 106;
export const COLOR_YUV2GRAY_YVYU = 124;
export const COLOR_YUV2RGB = 85;
export const COLOR_YUV2RGBA_I420 = 104;
export const COLOR_YUV2RGBA_IYUV = 104;
export const COLOR_YUV2RGBA_NV12 = 94;
export const COLOR_YUV2RGBA_NV21 = 96;
export const COLOR_YUV2RGBA_UYNV = 111;
export const COLOR_YUV2RGBA_UYVY = 111;
export const COLOR_YUV2RGBA_Y422 = 111;
export const COLOR_YUV2RGBA_YUNV = 119;
export const COLOR_YUV2RGBA_YUY2 = 119;
export const COLOR_YUV2RGBA_YUYV = 119;
export const COLOR_YUV2RGBA_YV12 = 102;
export const COLOR_YUV2RGBA_YVYU = 121;
export const COLOR_YUV2RGB_I420 = 100;
export const COLOR_YUV2RGB_IYUV = 100;
export const COLOR_YUV2RGB_NV12 = 90;
export const COLOR_YUV2RGB_NV21 = 92;
export const COLOR_YUV2RGB_UYNV = 107;
export const COLOR_YUV2RGB_UYVY = 107;
export const COLOR_YUV2RGB_Y422 = 107;
export const COLOR_YUV2RGB_YUNV = 115;
export const COLOR_YUV2RGB_YUY2 = 115;
export const COLOR_YUV2RGB_YUYV = 115;
export const COLOR_YUV2RGB_YV12 = 98;
export const COLOR_YUV2RGB_YVYU = 117;
export const COLOR_YUV420p2BGR = 99;
export const COLOR_YUV420p2BGRA = 103;
export const COLOR_YUV420p2GRAY = 106;
export const COLOR_YUV420p2RGB = 98;
export const COLOR_YUV420p2RGBA = 102;
export const COLOR_YUV420sp2BGR = 93;
export const COLOR_YUV420sp2BGRA = 97;
export const COLOR_YUV420sp2GRAY = 106;
export const COLOR_YUV420sp2RGB = 92;
export const COLOR_YUV420sp2RGBA = 96;
export const COLOR_mRGBA2RGBA = 126;

// ---- THRESH ----
export const THRESH_BINARY = 0;
export const THRESH_BINARY_INV = 1;
export const THRESH_DRYRUN = 128;
export const THRESH_MASK = 7;
export const THRESH_OTSU = 8;
export const THRESH_TOZERO = 3;
export const THRESH_TOZERO_INV = 4;
export const THRESH_TRIANGLE = 16;
export const THRESH_TRUNC = 2;

// ---- MORPH ----
export const MORPH_BLACKHAT = 6;
export const MORPH_CLOSE = 3;
export const MORPH_CROSS = 1;
export const MORPH_DIAMOND = 3;
export const MORPH_DILATE = 1;
export const MORPH_ELLIPSE = 2;
export const MORPH_ERODE = 0;
export const MORPH_GRADIENT = 4;
export const MORPH_HITMISS = 7;
export const MORPH_OPEN = 2;
export const MORPH_RECT = 0;
export const MORPH_TOPHAT = 5;

// ---- RETR ----
export const RETR_CCOMP = 2;
export const RETR_EXTERNAL = 0;
export const RETR_FLOODFILL = 4;
export const RETR_LIST = 1;
export const RETR_TREE = 3;

// ---- CHAIN_APPROX ----
export const CHAIN_APPROX_NONE = 1;
export const CHAIN_APPROX_SIMPLE = 2;
export const CHAIN_APPROX_TC89_KCOS = 4;
export const CHAIN_APPROX_TC89_L1 = 3;

// ---- INTER ----
export const INTER_AREA = 3;
export const INTER_CUBIC = 2;
export const INTER_LANCZOS4 = 4;
export const INTER_LINEAR = 1;
export const INTER_LINEAR_EXACT = 5;
export const INTER_NEAREST = 0;
export const INTER_NEAREST_EXACT = 6;

// ---- BORDER ----
export const BORDER_CONSTANT = 0;
export const BORDER_DEFAULT = 4;
export const BORDER_ISOLATED = 16;
export const BORDER_REFLECT = 2;
export const BORDER_REFLECT101 = 4;
export const BORDER_REFLECT_101 = 4;
export const BORDER_REPLICATE = 1;
export const BORDER_TRANSPARENT = 5;
export const BORDER_WRAP = 3;

// ---- TM ----
export const TM_CCOEFF = 4;
export const TM_CCOEFF_NORMED = 5;
export const TM_CCORR = 2;
export const TM_CCORR_NORMED = 3;
export const TM_SQDIFF = 0;
export const TM_SQDIFF_NORMED = 1;

// ---- NORM ----
export const NORM_HAMMING = 6;
export const NORM_HAMMING2 = 7;
export const NORM_INF = 1;
export const NORM_L1 = 2;
export const NORM_L2 = 4;
export const NORM_L2SQR = 5;
export const NORM_MINMAX = 32;
export const NORM_RELATIVE = 8;

// ---- CMP ----
export const CMP_EQ = 0;
export const CMP_GE = 2;
export const CMP_GT = 1;
export const CMP_LE = 4;
export const CMP_LT = 3;
export const CMP_NE = 5;

// ---- FONT_HERSHEY ----
export const FONT_HERSHEY_COMPLEX = 3;
export const FONT_HERSHEY_COMPLEX_SMALL = 5;
export const FONT_HERSHEY_DUPLEX = 2;
export const FONT_HERSHEY_PLAIN = 1;
export const FONT_HERSHEY_SCRIPT_COMPLEX = 7;
export const FONT_HERSHEY_SCRIPT_SIMPLEX = 6;
export const FONT_HERSHEY_SIMPLEX = 0;
export const FONT_HERSHEY_TRIPLEX = 4;

// ---- LINE ----
export const LINE_4 = 4;
export const LINE_8 = 8;
export const LINE_AA = 16;

// ---- COLORMAP ----
export const COLORMAP_AUTUMN = 0;
export const COLORMAP_BONE = 1;
export const COLORMAP_CIVIDIS = 17;
export const COLORMAP_COOL = 8;
export const COLORMAP_DEEPGREEN = 21;
export const COLORMAP_HOT = 11;
export const COLORMAP_HSV = 9;
export const COLORMAP_INFERNO = 14;
export const COLORMAP_JET = 2;
export const COLORMAP_MAGMA = 13;
export const COLORMAP_OCEAN = 5;
export const COLORMAP_PARULA = 12;
export const COLORMAP_PINK = 10;
export const COLORMAP_PLASMA = 15;
export const COLORMAP_RAINBOW = 4;
export const COLORMAP_SPRING = 7;
export const COLORMAP_SUMMER = 6;
export const COLORMAP_TURBO = 20;
export const COLORMAP_TWILIGHT = 18;
export const COLORMAP_TWILIGHT_SHIFTED = 19;
export const COLORMAP_VIRIDIS = 16;
export const COLORMAP_WINTER = 3;

// ---- DIST ----
export const DIST_C = 3;
export const DIST_FAIR = 5;
export const DIST_HUBER = 7;
export const DIST_L1 = 1;
export const DIST_L12 = 4;
export const DIST_L2 = 2;
export const DIST_LABEL_CCOMP = 0;
export const DIST_LABEL_PIXEL = 1;
export const DIST_MASK_3 = 3;
export const DIST_MASK_5 = 5;
export const DIST_MASK_PRECISE = 0;
export const DIST_USER = -1;
export const DIST_WELSCH = 6;

// ---- HOUGH ----
export const HOUGH_GRADIENT = 3;
export const HOUGH_GRADIENT_ALT = 4;
export const HOUGH_MULTI_SCALE = 2;
export const HOUGH_PROBABILISTIC = 1;
export const HOUGH_STANDARD = 0;

// ---- KMEANS ----
export const KMEANS_PP_CENTERS = 2;
export const KMEANS_RANDOM_CENTERS = 0;
export const KMEANS_USE_INITIAL_LABELS = 1;

// ---- HISTCMP ----
export const HISTCMP_BHATTACHARYYA = 3;
export const HISTCMP_CHISQR = 1;
export const HISTCMP_CHISQR_ALT = 4;
export const HISTCMP_CORREL = 0;
export const HISTCMP_HELLINGER = 3;
export const HISTCMP_INTERSECT = 2;
export const HISTCMP_KL_DIV = 5;

// ---- CALIB ----
export const CALIB_CB_ACCURACY = 32;
export const CALIB_CB_ADAPTIVE_THRESH = 1;
export const CALIB_CB_ASYMMETRIC_GRID = 2;
export const CALIB_CB_CLUSTERING = 4;
export const CALIB_CB_EXHAUSTIVE = 16;
export const CALIB_CB_FAST_CHECK = 8;
export const CALIB_CB_FILTER_QUADS = 4;
export const CALIB_CB_LARGER = 64;
export const CALIB_CB_MARKER = 128;
export const CALIB_CB_NORMALIZE_IMAGE = 2;
export const CALIB_CB_PLAIN = 256;
export const CALIB_CB_SYMMETRIC_GRID = 1;
export const CALIB_CHECK_COND = 16777216;
export const CALIB_DISABLE_SCHUR_COMPLEMENT = 262144;
export const CALIB_FIX_ASPECT_RATIO = 2;
export const CALIB_FIX_FOCAL_LENGTH = 16;
export const CALIB_FIX_INTRINSIC = 256;
export const CALIB_FIX_K1 = 32;
export const CALIB_FIX_K2 = 64;
export const CALIB_FIX_K3 = 128;
export const CALIB_FIX_K4 = 2048;
export const CALIB_FIX_K5 = 4096;
export const CALIB_FIX_K6 = 8192;
export const CALIB_FIX_PRINCIPAL_POINT = 4;
export const CALIB_FIX_S1_S2_S3_S4 = 65536;
export const CALIB_FIX_SKEW = 33554432;
export const CALIB_FIX_TANGENT_DIST = 2097152;
export const CALIB_FIX_TAUX_TAUY = 524288;
export const CALIB_HAND_EYE_ANDREFF = 3;
export const CALIB_HAND_EYE_DANIILIDIS = 4;
export const CALIB_HAND_EYE_HORAUD = 2;
export const CALIB_HAND_EYE_PARK = 1;
export const CALIB_HAND_EYE_TSAI = 0;
export const CALIB_MODEL_FISHEYE = 1;
export const CALIB_MODEL_PINHOLE = 0;
export const CALIB_RATIONAL_MODEL = 16384;
export const CALIB_RECOMPUTE_EXTRINSIC = 8388608;
export const CALIB_ROBOT_WORLD_HAND_EYE_LI = 1;
export const CALIB_ROBOT_WORLD_HAND_EYE_SHAH = 0;
export const CALIB_SAME_FOCAL_LENGTH = 512;
export const CALIB_STEREO_REGISTRATION = 67108864;
export const CALIB_THIN_PRISM_MODEL = 32768;
export const CALIB_TILTED_MODEL = 262144;
export const CALIB_USE_EXTRINSIC_GUESS = 4194304;
export const CALIB_USE_INTRINSIC_GUESS = 1;
export const CALIB_USE_LU = 131072;
export const CALIB_USE_QR = 1048576;
export const CALIB_ZERO_DISPARITY = 1024;
export const CALIB_ZERO_TANGENT_DIST = 8;

// ---- WARP ----
export const WARP_FILL_OUTLIERS = 8;
export const WARP_INVERSE_MAP = 16;
export const WARP_POLAR_LINEAR = 0;
export const WARP_POLAR_LOG = 256;
export const WARP_RELATIVE_MAP = 32;

// ---- INPAINT ----
export const INPAINT_NS = 0;
export const INPAINT_TELEA = 1;

// ---- RANSAC ----
export const RANSAC = 8;

// ---- MISC ----
export const CONTOURS_MATCH_I1 = 1;
export const CONTOURS_MATCH_I2 = 2;
export const CONTOURS_MATCH_I3 = 3;
export const LMEDS = 4;
export const RHO = 16;

// ---- IMREAD_ (imread 的 flag; OpenCV 4.x 稳定值, opencv-js 的导出里没有这些名字) ----
export const IMREAD_UNCHANGED = -1;
export const IMREAD_GRAYSCALE = 0;
export const IMREAD_COLOR = 1;
export const IMREAD_ANYDEPTH = 2;
export const IMREAD_ANYCOLOR = 4;
export const IMREAD_IGNORE_ORIENTATION = 128;

// ---- ADAPTIVE_THRESH_ (adaptiveThreshold 的 method) ----
export const ADAPTIVE_THRESH_MEAN_C = 0;
export const ADAPTIVE_THRESH_GAUSSIAN_C = 1;

// ---- DECOMP_ (Mat.inv / cv::invert 的 method) ----
export const DECOMP_LU = 0;
export const DECOMP_SVD = 1;
export const DECOMP_EIG = 2;
export const DECOMP_CHOLESKY = 3;
export const DECOMP_QR = 4;
export const DECOMP_NORMAL = 16;

// ---- TERM_CRITERIA_ (迭代算法终止条件; opencv-js 的导出里没有, 但教程/KMEANS 里常写) ----
export const TERM_CRITERIA_COUNT = 1;
export const TERM_CRITERIA_MAX_ITER = 1;
export const TERM_CRITERIA_EPS = 2;

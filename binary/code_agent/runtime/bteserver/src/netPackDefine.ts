// ---------------------------------------------------------------------------
// net/netPackDefine.js —— 对应 C++ common/net/netPackDefine.h + netPackDefine.cpp
//
// NetPackVariant(透传字节的通用包) + NetPackRegist(注册包类型, 初始化工步名/
// 参数类型/参数限制/错误码表)。
//
// Qt -> JS: QDataStream(小端) -> ByteWriter/ByteReader(裸名 'bytes' 模块);
//           QFile + QTextStream 读 BTELogCode.txt -> fs.readFileSync(path, "utf8");
//           QSettings(option.ini) -> 引擎的 JZSettings(接口对齐, value() -> getInt());
//           qRegisterMetaType<NetPackPtr> -> 无对应物, 保留注释。
//
// 2026-09-14 去 Qt 兼容层: 原来的 QApplication/QFile/QSettings/QTextStream 全换成
//   引擎的真实现 —— 应用目录 jzApp.applicationDirPath(), 配置 JZSettings, 文本读 fs。
//
// 依赖说明: stru_BTS_CMD_SET_STEP76_Ex 由 ./structs.js 导出(见 structs.js 末尾的
// 工步包类包装, 0x0200 的 pack/unpack 仍是待收口项 —— 这里只用到它的
// StepInfo.ParamInfo 参数类型表, 供"设置工步参数类型/限制"使用)。
// ---------------------------------------------------------------------------

import fs from 'fs';
import bytes from 'bytes';             // 默认导入取成员(具名导入在 tsgo 下 TS2614, 见 protocol.ts 说明)
const ByteWriter = bytes.ByteWriter;
import { NetPack, NetPackManager, PackInfo } from './netPack.js';
import {
    PackDefine, PARAM_Current, PARAM_Voltage, PARAM_StopCurrent, PARAM_StopVoltage, PARAM_Time,
    PARAM_BeginStepID, PARAM_CountLoop,
    STEP_TYPE_1, STEP_TYPE_2, STEP_TYPE_3, STEP_TYPE_IDLE, STEP_TYPE_LOOP, STEP_TYPE_7,
} from './PackDefine.js';
import { stru_BTS_CMD_SET_STEP76_Ex } from './structs.js';

export const NETPACK_INVAILD = 0;
export const NETPACK_VARIANT = 1;

// ---------------------------------------------------------------------------
// class NetPackVariant : public NetPack
// ---------------------------------------------------------------------------
export class NetPackVariant extends NetPack
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // DECLAR_PACK(NetPackVariant, NETPACK_VARIANT);  -> 静态成员 + type() 覆写
    static netPackCreate() { return new NetPackVariant(); }
    type() { return NetPackVariant.NetType; }

    // NetPackVariant();
    constructor()
    {
        super();
        this.data = new Uint8Array(0);   // QByteArray data;
    }

    // virtual bool pack(QByteArray &buffer) const;
    pack(buffer)
    {
        // ByteWriter 的整块写入接口叫 raw(Qt 侧是 QDataStream::writeRawData)
        const out = new ByteWriter(this.data.length);
        out.raw(this.data);
        buffer.push(out.bytes());
        return true;
    }

    // virtual bool unPack(const QByteArray &buffer);
    unPack(buffer)
    {
        console.assert(buffer.length === this.info.bodyLength());   // Q_ASSERT(buffer.size() == info.bodyLength());
        this.data = buffer;
        return true;
    }
}
NetPackVariant.NetType = NETPACK_VARIANT;

// ---------------------------------------------------------------------------
// void NetPackRegist();
// ---------------------------------------------------------------------------
export function NetPackRegist()
{
    // qRegisterMetaType<NetPackPtr>("NetPackPtr");
    NetPackManager.instance().registPack(NetPackVariant);

    const nameList = PackDefine.StepNameList;
    nameList.push("未定义", "恒流充电", "恒流放电", "恒压充电",
        "静    置", "循    环", "结    束", "恒流恒压充电",
        "恒功率放电", "恒功率充电", "恒阻放电", "恒阻充电",
        "测量内阻", "挂起工步", "N闭环恒压充电", "N闭环恒流恒压充电",
        "脉冲工步", "模拟工步", "电池组恒流恒压", "恒压放电",
        "恒流恒压放电", "控制工步");

    // 原 QSettings(path, IniFormat) -> 引擎的 JZSettings: 先 open 再 getInt
    const set = new JZSettings();
    set.open(jzApp.applicationDirPath() + "/option.ini");
    const lang = set.getInt("lang");
    if (lang === 1)
    {
        nameList.length = 0;
        nameList.push("None", "CC_Chg", "CC_DChg", "CV_ Chg",
            "Rest", "Cycle", "End", "CCCV_Chg",
            "Power_DChg", "Power_Chg", "RI_DChg", "RI_Chg",
            "测量内阻", "挂起工步", "N闭环恒压充电", "N闭环恒流恒压充电",
            "脉冲工步", "模拟工步", "电池组恒流恒压", "CV_ DChg",
            "CCCV_DChg", "控制工步");
    }

    /*设置工步参数类型*/
    // using ParamInfo = stru_BTS_CMD_SET_STEP76_Ex::StepInfo::ParamInfo;
    const ParamInfo = stru_BTS_CMD_SET_STEP76_Ex.StepInfo.ParamInfo;
    const typeList = stru_BTS_CMD_SET_STEP76_Ex.StepInfo.typeList;
    typeList.length = 64;

    for (let i = 0; i < typeList.length; i++)
        typeList[i] = ParamInfo.TYPE_PARAM;
    typeList[31] = ParamInfo.TYPE_NONE;
    typeList[63] = ParamInfo.TYPE_NONE;

    /*设置工步参数限制*/
    const mastHave = PackDefine.StepTypeMustHave;
    mastHave.length = nameList.length;

    pushList(mastHave, STEP_TYPE_1, [PARAM_Current, PARAM_StopVoltage]);
    pushList(mastHave, STEP_TYPE_2, [PARAM_Current, PARAM_Voltage]);
    pushList(mastHave, STEP_TYPE_3, [PARAM_Voltage, PARAM_StopCurrent]);
    pushList(mastHave, STEP_TYPE_IDLE, [PARAM_Time]);
    pushList(mastHave, STEP_TYPE_LOOP, [PARAM_BeginStepID, PARAM_CountLoop]);
    pushList(mastHave, STEP_TYPE_7, [PARAM_Current, PARAM_Voltage, PARAM_StopCurrent]);

    /*错误信息*/
    // 原: QFile + QTextStream 逐行读。现在一次性读成文本再按行切(错误码表很小)。
    const logPath = jzApp.applicationDirPath() + "/Modules/BTELogCode.txt";
    if (fs.existsSync(logPath))
    {
        const lines = fs.readFileSync(logPath, "utf8").split(/\r?\n/);
        for (const line of lines)
        {
            const list = line.split("-");
            if (list.length > 2)
                PackDefine.ErrorList.set(parseInt(list[0], 16), list[list.length - 1]);   // list.back()
        }
    }
}

// C++: mastHave[STEP_TYPE_x] << PARAM_A << PARAM_B;  (QVector<QVector<int>> 逐项 push_back)
function pushList(mastHave, stepType, params)
{
    if (!mastHave[stepType])
        mastHave[stepType] = [];
    for (let i = 0; i < params.length; i++)
        mastHave[stepType].push(params[i]);
}

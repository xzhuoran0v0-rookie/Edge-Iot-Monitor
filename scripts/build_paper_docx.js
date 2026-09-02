const fs = require("fs");
const {
  Document, Packer, Paragraph, TextRun, Table, TableRow, TableCell,
  WidthType, AlignmentType, LineRuleType, HeadingLevel, Footer,
  PageNumber, BorderStyle, ShadingType, PageBreak,
} = require("docx");

// ---- 版式常量（电赛要求）----
const BODY = 24;        // 小四 = 12pt → half-points
const SMALL = 21;       // 五号 = 10.5pt
const H1 = 28;          // 四号 = 14pt
const H2 = 24;
const TITLE = 36;       // 小二 = 18pt
const LINE = 440;       // 固定行距 22 磅 = 22 × 20
const INDENT = 480;     // 首行缩进 2 字符 @12pt
const SONG = { ascii: "Times New Roman", eastAsia: "SimSun", hAnsi: "Times New Roman" };
const HEI = { ascii: "Arial", eastAsia: "SimHei", hAnsi: "Arial" };
const MONO = { ascii: "Consolas", eastAsia: "SimSun", hAnsi: "Consolas" };

const CONTENT_W = 9070; // A4 11906 − 左右各 1418

// 把 **粗体** 和 `代码` 拆成 run
function runs(text, { size = BODY, font = SONG } = {}) {
  const out = [];
  const re = /(\*\*[^*]+\*\*|`[^`]+`)/g;
  let last = 0, m;
  while ((m = re.exec(text)) !== null) {
    if (m.index > last) out.push(new TextRun({ text: text.slice(last, m.index), size, font }));
    const tok = m[0];
    if (tok.startsWith("**")) {
      out.push(new TextRun({ text: tok.slice(2, -2), size, font, bold: true }));
    } else {
      out.push(new TextRun({ text: tok.slice(1, -1), size: size - 2, font: MONO }));
    }
    last = m.index + tok.length;
  }
  if (last < text.length) out.push(new TextRun({ text: text.slice(last), size, font }));
  return out.length ? out : [new TextRun({ text: "", size, font })];
}

const p = (text, opts = {}) => new Paragraph({
  children: runs(text, opts),
  spacing: { line: LINE, lineRule: LineRuleType.EXACT, before: 0, after: opts.after ?? 0 },
  indent: opts.noIndent ? undefined : { firstLine: INDENT },
  alignment: opts.align,
});

const gap = () => new Paragraph({ children: [new TextRun({ text: "", size: 12 })],
  spacing: { line: 120, lineRule: LineRuleType.EXACT } });

const h1 = (t) => new Paragraph({
  children: [new TextRun({ text: t, size: H1, font: HEI, bold: true })],
  spacing: { line: LINE, lineRule: LineRuleType.EXACT, before: 240, after: 120 },
  heading: HeadingLevel.HEADING_1,
});

const h2 = (t) => new Paragraph({
  children: [new TextRun({ text: t, size: H2, font: HEI, bold: true })],
  spacing: { line: LINE, lineRule: LineRuleType.EXACT, before: 160, after: 80 },
  heading: HeadingLevel.HEADING_2,
});

// 等宽代码 / 图块
const code = (lines) => lines.map((l, i) => new Paragraph({
  children: [new TextRun({ text: l || " ", size: 18, font: MONO })],
  spacing: { line: 240, lineRule: LineRuleType.EXACT, before: i === 0 ? 60 : 0,
             after: i === lines.length - 1 ? 60 : 0 },
  indent: { left: 240 },
}));

// 表格：columnWidths 与每个 cell 的 width 都要给，单位 DXA
function table(header, rows, weights) {
  const total = weights.reduce((a, b) => a + b, 0);
  const cols = weights.map((w) => Math.round((w / total) * CONTENT_W));
  const cell = (text, opts = {}) => new TableCell({
    width: { size: cols[opts.i], type: WidthType.DXA },
    shading: opts.head ? { type: ShadingType.CLEAR, fill: "F2F2F2" } : undefined,
    margins: { top: 40, bottom: 40, left: 80, right: 80 },
    children: [new Paragraph({
      children: runs(text, { size: SMALL }).map((r) => r),
      spacing: { line: 260, lineRule: LineRuleType.EXACT },
      alignment: opts.head ? AlignmentType.CENTER : undefined,
    })],
  });
  return new Table({
    columnWidths: cols,
    width: { size: CONTENT_W, type: WidthType.DXA },
    rows: [
      new TableRow({
        tableHeader: true,
        children: header.map((t, i) => cell(t, { i, head: true })),
      }),
      ...rows.map((r) => new TableRow({ children: r.map((t, i) => cell(t, { i })) })),
    ],
  });
}

const children = [];

// ---------------- 标题与摘要 ----------------
children.push(new Paragraph({
  children: [new TextRun({ text: "边缘智能环境监测与报警系统", size: TITLE, font: HEI, bold: true })],
  alignment: AlignmentType.CENTER,
  spacing: { line: 520, lineRule: LineRuleType.EXACT, after: 200 },
}));

children.push(new Paragraph({
  children: [new TextRun({ text: "摘要", size: BODY, font: HEI, bold: true })],
  spacing: { line: LINE, lineRule: LineRuleType.EXACT, after: 60 },
}));

children.push(p("针对环境监测的两类失效——固定阈值不适应现场本底环境、云端判决在断网时失去告警能力——本系统将判决逻辑完整下沉至 ESP32-S3。设备以 1 Hz 独立任务采样 SHT30，由自适应基线学习安装现场的正常范围，与 12 样本窗口的固定阈值及趋势速率共同给出状态、严重度、置信度与原因码；越限时蜂鸣器与 OLED 在 1 秒内本地告警，判决路径无任何网络调用。同一结论再经 MQTTS 上报华为云 IoTDA 并存入本地 SQLite。自适应带被钳制在固定安全限内侧，学习只能收窄关注范围，不能放宽安全边界。实测断网与冷启动下告警不受影响，占用 RAM 14.5%、Flash 14.7%。"));

children.push(gap());
children.push(p("关键词：边缘计算；自适应基线；确定性告警；ESP32-S3；华为云 IoTDA", { noIndent: true }));

// ---------------- 1 ----------------
children.push(h1("1  方案设计与论证"));
children.push(h2("1.1  问题分析"));
children.push(p("温湿度监测的告警环节存在两个独立的失效模式。其一，**固定阈值不具备场所适应性**：同一套阈值用于常温机房与锅炉房，前者误报、后者漏报，而设备无法预先知道安装现场的本底环境。其二，**判决依赖网络则在断网时失效**，而断网恰恰与供电异常、设备故障等需要告警的场景高度相关——系统最需要工作的时刻，正是它最可能失效的时刻。"));
children.push(p("近年常见的“云端大模型分析”方案同时继承了这两个问题：每一次判决都依赖连通性、API 配额，以及模型返回可解析结构化输出的能力。"));

children.push(h2("1.2  方案比较与选择"));
children.push(p("围绕“判决在哪里做”，比较三种方案："));
children.push(table(
  ["对比项", "方案一：云端判决", "方案二：纯固定阈值", "方案三：片上自适应判决"],
  [
    ["断网可用", "否", "是", "是"],
    ["场所适应性", "依赖提示词", "无", "有，且有界"],
    ["告警时延", "往返 + 推理，不可控", "< 1 s", "< 1 s"],
    ["单次判决成本", "按 API 计费", "0", "0"],
    ["误报抑制", "依赖模型", "无", "趋势 + 迟滞 + 不可信判定"],
  ], [16, 22, 20, 26]));
children.push(gap());
children.push(p("方案一在演示中效果好，但把告警的可用性绑定在三个外部条件上；方案二可靠但不解决适应性问题。"));
children.push(p("**本系统选择方案三**：判决全部由 MCU 上的确定性代码完成，网络只负责把已经得出的结论送出去，从不参与产生结论。这一选择的代价是设备端计算与存储开销，实测该开销可忽略（4.4 节）。"));
children.push(p("本系统仍保留一个大语言模型接口，但其职责被严格限定为把状态变化翻译成人类可读的语句。代码中不存在任何将模型输出转换为告警或控制指令的路径，该功能默认关闭，关闭后系统监测与告警功能完整。"));

children.push(h2("1.3  系统总体方案"));
children.push(...code([
  "        +--------------------------------------------+",
  "        |  ESP32-S3                  [唯一判决层]     |",
  " SHT30->|  1 Hz 安全任务: 物理有效性 + 硬限 + 告警阈值 |",
  "        |  中值滤波 -> AdaptiveBaseline (学习正常范围) |",
  "        |           -> EdgeReasoner (阈值 + 趋势速率)  |",
  "        |  => EdgeAssessment {状态,严重度,置信度,原因} |",
  "        +---+-------------+--------------+------------+",
  "            |             |              |",
  "     +------v-----+ +-----v------+ +-----v----------+",
  "     | 蜂鸣器+OLED| |   IoTDA    | | 本地服务        |",
  "     | <1s 无网络 | |   MQTTS    | | SQLite + IQR   |",
  "     +------------+ +------------+ +----------------+",
  "      [告警出口]      [云端记录]     [记录/验证夹具]",
]));
children.push(p("判决与记录严格分离：蜂鸣器响起时，后两条通路尚未开始工作；两条通路任一失效或同时失效，均不影响已经发生的告警。"));

// ---------------- 2 ----------------
children.push(h1("2  理论分析与计算"));
children.push(h2("2.1  自适应基线的带宽计算"));
children.push(p("设第 k 个被接受样本为 x(k)，中心 μ 与平均偏差 σ 按指数移动平均递推："));
children.push(...code([
  "μ(k) = μ(k-1) + α · [ x(k) - μ(k-1) ]",
  "σ(k) = σ(k-1) + β · [ |x(k) - μ(k-1)| - σ(k-1) ]",
]));
children.push(p("取 α = 0.02、β = 0.05。学习率取小值使单个样本对中心的影响不超过 2%，正常昼夜漂移不会显著拖动基线。判定带为："));
children.push(...code([
  "[ μ - w , μ + w ] ,   w = clamp( 3σ , w_min , w_max )",
  "温度  w ∈ [1.5, 5.0] °C     湿度  w ∈ [5, 15] %RH",
]));
children.push(p("上界的必要性在于：无界的 3σ 在长期噪声下会持续扩张，最终接受任何输入。"));

children.push(h2("2.2  有界性：学习不能放宽安全边界"));
children.push(p("设固定硬限为 [L, U]（温度 −10…45 °C，湿度 5…95 %RH），保护间隔 g（温度 0.5 °C，湿度 2 %RH）。实际判定带取："));
children.push(...code([
  "[ max( μ - w , L + g ) ,  min( μ + w , U - g ) ]",
]));
children.push(p("由该式直接得到：对任意学习历史，判定带上界恒有 μ + w ≤ U − g < U。**即无论基线学到什么，都不可能把判定边界推到硬限之外。**"));
children.push(p("更强的一条来自系统分层：告警阈值与硬限均为编译期常量，不参与任何学习过程，蜂鸣器由原始读数直接与其比较驱动。因此基线学习**只改变哪些读数被称为“模式偏移”，不改变哪些读数触发告警**。这使得允许基线移动这件事本身是安全的。"));

children.push(h2("2.3  搬迁与异常的区分"));
children.push(p("带外样本不参与学习，可防止持续异常被同化为正常。但若仅有该规则，设备被迁至新环境后将出现：所有样本恒在带外，无学习，带永不移动，永久输出 BASELINE_SHIFT，且持久化路径因“无学习即无脏数据”而永不触发。"));
children.push(p("故引入判据：连续带外样本数 n ≥ N(r) 时判定为环境迁移，清零重新学习；任一带内样本使 n 归零。N(r) 不取常数，而由感知周期反推使其恒等于 1 小时（见 3.4 节），因此改变采样率不会改变“多久算持续偏离”这一语义。由 2.2 节结论，该机制不影响告警行为。"));

children.push(h2("2.4  趋势速率判据"));
children.push(p("对 12 样本窗口首末样本，按分钟归一化："));
children.push(...code([
  "v = [ x(n-1) - x(0) ] / [ ( t(n-1) - t(0) ) / 60 ]",
]));
children.push(p("取 v ≥ 1.2 °C/min 且净升 ≥ 0.8 °C 判为温度快升。按速率而非绝对变化量判定的原因是：8 °C 的变化在 1 小时内属常态，在 8 秒内对室内空气不具物理合理性，二者仅由速率区分。"));

children.push(h2("2.5  告警时延"));
children.push(p("告警判据在 1 Hz 安全任务内求值，该任务在网络初始化之前创建。最坏时延为 t(max) = T(sample) + t(GPIO) ≈ 1 s。若将求值置于主循环，则须等待 setup() 中 Wi-Fi（超时 20 s）、NTP、MQTT 全部完成，最坏时延超过 30 s。二者差异已实测验证（4.3 节）。"));

// ---------------- 3 ----------------
children.push(h1("3  电路与程序设计"));
children.push(h2("3.1  硬件电路"));
children.push(table(
  ["器件", "接口", "引脚", "地址"],
  [
    ["SHT30 温湿度传感器", "I²C 总线 0（Wire）", "SDA 17 / SCL 18", "0x44"],
    ["OLED 128×64（SSD1315）", "I²C 总线 1（Wire1）", "SDA 38 / SCL 39", "0x3C"],
    ["蜂鸣器", "GPIO", "GPIO 4，低电平有效", "—"],
    ["翻页按键", "GPIO", "GPIO 0（板载 BOOT）", "—"],
  ], [30, 26, 28, 16]));
children.push(gap());
children.push(p("**两器件置于独立 I²C 总线**，而非共用一条。原因是显示器件在异常状态下可能长时间占用总线，若与传感器共用，将阻塞 1 Hz 安全采样。该分离使显示故障不能影响告警链路。"));
children.push(p("蜂鸣器由编译期开关控制，关闭时引脚置为高阻输入，任何远程命令都无法使其带电，以避免未经验证的电路被误驱动。"));

children.push(h2("3.2  软件流程"));
children.push(...code([
  "setup():  蜂鸣器静音 -> OLED 清屏 + 自检 -> 串口 -> 基线载入(NVS)",
  "          -> 创建 1 Hz 安全任务 (先于 Wi-Fi) -> Wi-Fi/NTP/MQTTS",
  "",
  "安全任务 (1 Hz, 独立于网络):",
  "  读 SHT30 -> 物理有效性 -> 硬限 -> 告警阈值 -> 蜂鸣器/OLED",
  "",
  "主循环 (2 s):",
  "  取安全快照(超时 1.5 s 拒绝上报) -> 中值滤波 -> 基线 -> 叠加",
  "  -> OLED 刷新 -> HTTP 上报 -> 命令轮询",
  "  每 10 s 向 EdgeReasoner 输入样本；每 60 s 发一次 MQTTS",
]));

children.push(h2("3.3  状态判据"));
children.push(p("EdgeReasoner 按顺序求值，首个命中者生效，完整判据见附录 A。依次为：读数不可信（UNSTABLE）、高温高湿并发、温度快升、湿度快升、温度偏高、湿度偏高，均不满足则为 NORMAL；样本不足 4 个时报 WARMUP。"));
children.push(p("UNSTABLE 置于首位是有意的：若信号本身不可信，由其导出的任何结论同样不可信，此时系统应当声明“读数不可信”，而非给出一个由噪声算出的确定结论。"));
children.push(p("自适应层叠加两个状态：HARD_LIMIT（覆盖一切）与 BASELINE_SHIFT（仅在固定层判为 NORMAL 时生效）。真实越限不会被降级为“模式偏移”。"));

children.push(h2("3.4  采样与上报周期的分离"));
children.push(p("系统内共有四个速率，各自由其物理或经济约束决定，不合并为一个参数："));
children.push(table(
  ["周期", "取值", "驱动", "约束来源"],
  [
    ["安全采样", "1 s", "硬限与告警阈值判定，驱动蜂鸣器", "告警时延要求"],
    ["SENSE_INTERVAL_MS", "2 s", "OLED 刷新、本地服务上报", "SHT30 响应 τ63≈2 s，更快无新信息"],
    ["EDGE_SAMPLE_INTERVAL_MS", "10 s", "向 EdgeReasoner 输入样本", "12 样本 × 10 s = 2 min 趋势窗口"],
    ["CLOUD_INTERVAL_MS", "60 s", "IoTDA 属性上报", "云平台消息配额"],
  ], [26, 10, 30, 34]));
children.push(gap());
children.push(p("合并为单一参数将迫使这些约束相互妥协。以感知与上报为例：缩短单一参数可提升显示响应，但云端消息量同比上升。分离后每日上报 1 440 条，占免费额度（10 000 条/日）的 14%，同一额度可支撑约 6 台设备；若取 10 s，单台即占用 86%，第二台设备便会超限。"));
children.push(p("推理输入之所以另设一档，是因为 EdgeReasoner 的窗口按**样本数**固定为 12，而其阈值是按 **2 分钟**窗口整定的。若直接按感知周期输入，把感知周期由 10 s 缩至 2 s 即等于把窗口缩至 24 s：阈值未变而含义已变——净升条件将取代速率条件成为实际门槛，判定“快速上升”所需的斜率反而由 1.2 °C/min 抬高至约 2 °C/min。"));
children.push(p("同理，基线的预热样本数与重学判据在实现上按样本计数，其语义却是时长，二者均由感知周期反推得出（预热 4 min、重学 1 h），从而在感知周期改变时保持不变。"));

children.push(h2("3.5  云端接口"));
children.push(p("设备按 IoTDA 官方主题与 JSON 结构上报，单条消息含两个 service：Environment 承载 temperature 与 humidity，EdgeReasoning 承载 state、severity、confidence 与 reason_code。"));
children.push(p("EdgeReasoning 即设备自身的判决结论。云端与本地服务均**原样存储与展示，不重新计算**——设备掌握云端不具备的传感历史与学习基线，若云端独立判决并与之不一致，将导致现场显示与远程显示相互矛盾。"));

// ---------------- 4 ----------------
children.push(h1("4  测试方案与测试结果"));
children.push(h2("4.1  测试条件"));
children.push(p("室温 24–27 °C，相对湿度 43–48 %RH；告警阈值 30 °C / 70 %RH；感知周期 2 s，云端上报周期 60 s。"));
children.push(p("激励方式：手掌包覆传感器。需说明的是，掌面接近饱和湿度，包覆后**湿度先于温度越限**——SHT30 湿度响应 τ63 ≈ 8 s，温度 τ63 ≈ 2 s，但掌内空气湿度在数秒内即达 80 %RH 以上，而热量需经外壳传导。故常温下该激励触发的是湿度告警。若需触发温度告警，改用热源置于传感器 1–2 cm 外（给热不给水汽）。"));

children.push(h2("4.2  单元测试（主机）"));
children.push(p("自适应基线逻辑不含 Arduino 依赖，可在主机直接编译运行。七项断言全部通过：学习与带宽上下界、首样本异常不污染基线、持久化往返与写入节流、复位后强制持久化、窄配置与 millis 回绕、持续偏离触发重学而短暂偏离不触发、重学后硬限仍然有效。"));
children.push(p("后端 HTTP 接口另有冒烟测试，覆盖设备白名单、量程校验、判决结论往返存取、命令鉴权与参数区间、叙述触发条件，全部通过。"));

children.push(h2("4.3  系统联试"));
children.push(p("**告警功能与网络无关性。** 关闭 Wi-Fi 后施加激励，蜂鸣器与 OLED 行为与联网时无差异。为在室温下复现“冷启动即越限”这一最不利工况，将告警阈值临时下调至 20 °C（低于当时室温 24.2 °C）后复位设备，串口输出为："));
children.push(...code([
  "[SAFETY] Dedicated 1 Hz task started",
  "[WiFi] Connecting to ...",
  "[ALARM] buzzer on, OLED: ALARM  TEMP 24.2C LIMIT 20.0C   <- 网络尚未连通",
  "[WiFi] Connected!",
]));
children.push(p("告警发生在 Wi-Fi 连接完成之前。作为对照，将同一判据置于主循环的版本中，该行出现在 [MQTT] Connected 之后，即需额外等待 Wi-Fi、NTP、MQTT 三段连接。验证后阈值已复原为 30 °C。"));
children.push(p("**云端上报。** 设备经 MQTTS(8883) 接入 IoTDA，控制台可见两个 service 的属性数据，串口对应输出 [IoTDA] Combined publish OK。"));
children.push(p("**存储行为。** 设备每个感知周期上报一次判决，而 edge_assessments 表仅记录状态变迁：实测 8 次上报产生 3 行记录，每行时间戳即该状态的起始时刻。"));
children.push(p("**降级行为。** 本地服务不可达时按指数退避重试（10 s 起，倍增至 5 min 封顶），推理、告警与云端上报均不受影响，且不在 OLED 上呈现——本地服务是验证夹具而非数据链路的一环，把它的缺席显示为设备状态会错误传达故障位置。传感器读数无效或快照超过 1.5 s 时阻断上报，而非发送陈旧值。"));

children.push(h2("4.4  资源占用"));
children.push(p("构建工具链接后直接给出（数值可由 pio run 复现）："));
children.push(table(
  ["项目", "主要构成", "占用", "容量", "比例"],
  [
    ["RAM（静态）", ".dram0.data + .dram0.bss", "47 372 B", "327 680 B", "14.5 %"],
    ["Flash", ".flash.text + .flash.rodata + .iram0.text + .dram0.data + 向量表", "963 673 B", "6 553 600 B", "14.7 %"],
  ], [16, 42, 15, 15, 12]));
children.push(gap());
children.push(p("两点须说明：RAM 一列为**链接期静态分配**，不含运行时堆与任务栈（安全任务栈另计 4 KB）；Flash 分母为分区表 default_16MB.csv 中 app0 分区容量 0x640000 = 6.25 MB，而非 16 MB 整片。"));
children.push(p("推理本身的增量开销可单独估计：12 样本环形缓冲共 12 × (2 × float + uint32) = 144 B，自适应基线状态 6 个 float 加计数器不足 40 B，合计不到 0.2 KB，其余占用来自 Wi-Fi/TLS/MQTT 协议栈。"));
children.push(p("单设备每日产生 43 200 次本地判决（2 s 周期），全部在设备上完成，无计费开销；上行云端消息 1 440 条，占免费额度 14%。"));

// ---------------- 5 ----------------
children.push(h1("5  结果分析与结论"));
children.push(p("实测表明，将判决下沉至 MCU 的方案达到了设计目标。**告警可用性与网络解耦**：断网、云端不可达、冷启动进入异常环境三种条件下，告警时延与联网时一致（≤ 1 s）；这一性质来自架构而非补救，判决路径中不存在网络调用。**适应性与安全性可以共存**：自适应带被钳制在固定硬限内侧，且告警阈值不参与学习，因此允许基线随环境移动不会削弱任何安全保证——会自适应的部分与提供保证的部分被显式分开，且前者被后者约束。**开销可忽略**：RAM 与 Flash 占用均低于 15%，其中推理本身不足 0.2 KB。"));
children.push(p("尚未完成的部分如实列出：经 IoTDA 应用侧接口下发命令尚未实现（当前命令由本地服务队列下发，设备侧接收与回执已实现并验证）；可视化仅有本地网页，尚未部署至云端；设备侧 Wi-Fi、MQTT、NTP 状态仅在 OLED 显示，未纳入上报报文；系统按单设备设计，数据模型虽以设备号为键，但无分组与告警分级策略。"));
children.push(p("后续工作方向：将 IoTDA 转发接入云端看板以摆脱同网段依赖；补齐应用侧命令下发；扩展多设备管理与告警分级；将开发板与模块整合为单块印制板，以消除接线引入的故障点。"));

// 正文到此结束，附录另起一页（电赛限制的是正文页数）
children.push(new Paragraph({ children: [new PageBreak()] }));

children.push(h1("附录 A  判据与主要参数"));
children.push(p("EdgeReasoner 状态判据（按顺序求值，首个命中生效）：", { noIndent: true }));
children.push(table(
  ["状态", "严重度", "条件"],
  [
    ["WARMUP", "info", "样本 < 4"],
    ["UNSTABLE", "warning", "窗口极差 ≥ 4 °C 或 ≥ 15 %RH，或相邻步进 ≥ 2.5 °C 或 ≥ 10 %RH"],
    ["HEAT_HUMID_RISK", "warning", "≥ 30 °C 且 ≥ 70 %RH"],
    ["TEMP_RISING", "warning", "≥ 1.2 °C/min 且净升 ≥ 0.8 °C"],
    ["HUMIDITY_RISING", "warning", "≥ 4 %RH/min 且净升 ≥ 3 %RH"],
    ["HIGH_TEMPERATURE / HIGH_HUMIDITY", "watch", "≥ 30 °C / ≥ 70 %RH"],
    ["NORMAL", "info", "以上均不满足"],
  ], [30, 14, 56]));
children.push(gap());
children.push(p("主要参数：", { noIndent: true }));
children.push(table(
  ["参数", "取值", "位置"],
  [
    ["安全采样 / 感知 / 推理输入 / 云端上报", "1 s / 2 s / 10 s / 60 s", "SENSE_ 与 CLOUD_INTERVAL_MS 等"],
    ["快照最大陈旧时间", "1.5 s", "SAFETY_SAMPLE_MAX_AGE_MS"],
    ["推理窗口", "12 样本（跨 2 min）", "WINDOW_SIZE"],
    ["基线预热 / 重学判据", "4 min / 连续带外 1 h", "按感知周期折算样本数"],
    ["学习率（中心 / 偏差）", "0.02 / 0.05", "centerLearningRate 等"],
    ["告警阈值", "30 °C / 70 %RH", "ALARM_TEMP_C 等"],
    ["固定硬限 / 保护间隔", "−10…45 °C、5…95 %RH / 0.5 °C、2 %RH", "hardTempLowerC 等"],
  ], [30, 32, 38]));

children.push(h1("附录 B  参考文献"));
[
  "[1] 华为云 IoTDA 设备属性上报接口文档.",
  "[2] Sensirion. SHT3x-DIS Datasheet.",
  "[3] Solomon Systech. SSD1315 Datasheet.",
  "[4] Espressif. ESP32-S3 Technical Reference Manual.",
].forEach((t) => children.push(p(t, { noIndent: true })));

// ---------------- 文档 ----------------
const doc = new Document({
  styles: { default: { document: { run: { font: SONG, size: BODY } } } },
  sections: [{
    properties: {
      page: {
        margin: { top: 1701, bottom: 1418, left: 1418, right: 1418 }, // 上 3cm
      },
    },
    footers: {
      default: new Footer({
        children: [new Paragraph({
          alignment: AlignmentType.RIGHT,
          children: [new TextRun({ children: [PageNumber.CURRENT], size: SMALL, font: SONG })],
        })],
      }),
    },
    children,
  }],
});

Packer.toBuffer(doc).then((buf) => {
  fs.writeFileSync(process.argv[2], buf);
  console.log("written:", process.argv[2], buf.length, "bytes");
});

// mldsa_ntt_core —— ML-DSA 的 256 点 NTT / INTT 核（BRAM 版：2 周期一个蝶形）
//
// 与 ML-KEM 侧的 ntt_core 结构相同，数学不同：
//   模数 q = 8380417（23 位），系数用 32 位有符号承载
//   Montgomery 基是 2³²，旋转因子表 256 项
//   **做满 8 层**（len 从 128 降到 1），因此 NTT 域的乘法是逐点标量乘，
//   不像 ML-KEM 那样停在 7 层、还要做一次 2×2 的基乘
//
// 正变换末尾没有统一归约（参考实现也没有）；逆变换末尾乘 f = mont²/256 = 41978，
// 所以 invntt(ntt(x)) ≡ x·2³² (mod q)，**不是恒等** —— 这一条最容易被误判成缺陷。
//
// 接口做成"写系数 → start → 等 done → 读系数"，与 pqchsm/accel.h 的寄存器语义
// 对得上，这样仿真出来的核可以直接挂到 accel transport 上。
//
// **done 是电平不是脉冲**：置位后一直保持，直到下一次 start（或复位）才清。
// accel.h 的契约是软件轮询 STATUS.DONE，而真实寄存器/AXI 轮询在任意时刻采样
// 会漏掉 1 周期脉冲，所以核这一侧就要给出可锁存的电平。
//
// ⚠️ **读口是同步读，有一拍延迟**：系数存储是 BRAM（common/ram_dp.v），
// 而 BRAM 没有组合读口。给出 rd_addr 之后要等一个上升沿，rd_data 才是那个地址的内容。
// 理由与 ML-KEM 侧那一版相同：256×32 的寄存器阵列加多个组合读口，
// 综合出来是几万个 LUT 的选择树，一颗 ZU3EG 放不下 —— 见 docs/TESTING.md 的 S3。
//
// ============================ 流水化改造 ============================
// 起因是实测：组合蝶形版的 ML-DSA-87 Sign，post-route
//   WNS @100MHz = −3.332ns  →  Fmax ≈ 75.0MHz，而系统时钟正好 75MHz，
//   余量 ≈ +0.001ns，进整体设计必挂。
// 关键路径 u_ntt/k_reg → u_ntt/u_mem/DINADIN[29]，逻辑层级 37：
// 旋转因子 ROM 的 256:1 选择树、ζ·b、mont 里的两次乘法、蝶形加减，全串在一拍里。
//
// 改造分两件事：
//
// 一、蝶形切成 5 级流水（mldsa_butterfly_pipe），ROM 读出也打一拍，
//    ζ 的取负挪到 ROM 之后的那一拍。CT / GS / 缩放合并成**一条**乘法链
//    （原来三者各例化一份 mont_reduce，等于三套乘法器）。
//
// 二、存储从"一块 BRAM 就地做"改成**乒乓两块**：一层里只读 buf[sel]、只写 buf[~sel]，
//    层末把 sel 翻过来。这样做的理由不是省事，是**从结构上消灭 RAW**——
//    流水化之后同一层内先写的地址可能被后读命中，而读源与写目的分属两块存储时
//    这件事根本不会发生，不需要旁路，也不需要"蝶形对地址跨度大于流水深度"这种
//    随参数变化的脆弱前提。
//    正确性还依赖一条已验证的性质（scratch 里用生成器逐层枚举确认，16 层全中）：
//      **每一层的 128 个蝶形恰好覆盖 [0,256) 的全部 256 个地址，无重复**，
//    所以目的 buf 每个地址恰好被写一次，不会留空洞。
//    代价是多一块 256×32（半个 BRAM tile）。
//
//    层与层之间**必须排空流水**：下一层要读的正是上一层刚写完的那块。
//    排空由 S_DRAIN 等 pipe_empty 完成，等价于 6 拍。
//
// 吞吐顺带从 2 拍/蝶形变成 1 拍/蝶形：
//   正变换 2049 → 1081 cycles，逆变换 2561 → 1344 cycles（test_mldsa_ntt 实测）。
`default_nettype none

module mldsa_ntt_core (
    input  wire               clk,
    input  wire               rst_n,

    input  wire               start,      // 单周期脉冲
    input  wire               inverse,    // 0 = 正变换，1 = 逆变换
    output reg                done,

    // 系数写口（done 之后 / start 之前用）
    input  wire               wr_en,
    input  wire  [7:0]        wr_addr,
    input  wire signed [31:0] wr_data,

    // 系数读口（**同步读，一拍延迟**）
    input  wire  [7:0]        rd_addr,
    output wire signed [31:0] rd_data
);

    localparam signed [31:0] FINV = 32'sd41978;     // mont²/256，只有 S_SCALE 用

    // ---- 旋转因子 ROM（Montgomery 域，与参考实现的表一致）----
    // zetas[i] = 1753^brv8(i) · 2³² mod q，折算到 (−q/2, q/2]。
    // 下标 0 在正逆变换里都取不到，与参考实现一样置 0。
    reg signed [31:0] zetas [0:255];
    initial begin
        zetas[0]=32'sd0;           zetas[1]=32'sd25847;       zetas[2]=-32'sd2608894;
        zetas[3]=-32'sd518909;     zetas[4]=32'sd237124;      zetas[5]=-32'sd777960;
        zetas[6]=-32'sd876248;     zetas[7]=32'sd466468;      zetas[8]=32'sd1826347;
        zetas[9]=32'sd2353451;     zetas[10]=-32'sd359251;    zetas[11]=-32'sd2091905;
        zetas[12]=32'sd3119733;    zetas[13]=-32'sd2884855;   zetas[14]=32'sd3111497;
        zetas[15]=32'sd2680103;    zetas[16]=32'sd2725464;    zetas[17]=32'sd1024112;
        zetas[18]=-32'sd1079900;   zetas[19]=32'sd3585928;    zetas[20]=-32'sd549488;
        zetas[21]=-32'sd1119584;   zetas[22]=32'sd2619752;    zetas[23]=-32'sd2108549;
        zetas[24]=-32'sd2118186;   zetas[25]=-32'sd3859737;   zetas[26]=-32'sd1399561;
        zetas[27]=-32'sd3277672;   zetas[28]=32'sd1757237;    zetas[29]=-32'sd19422;
        zetas[30]=32'sd4010497;    zetas[31]=32'sd280005;     zetas[32]=32'sd2706023;
        zetas[33]=32'sd95776;      zetas[34]=32'sd3077325;    zetas[35]=32'sd3530437;
        zetas[36]=-32'sd1661693;   zetas[37]=-32'sd3592148;   zetas[38]=-32'sd2537516;
        zetas[39]=32'sd3915439;    zetas[40]=-32'sd3861115;   zetas[41]=-32'sd3043716;
        zetas[42]=32'sd3574422;    zetas[43]=-32'sd2867647;   zetas[44]=32'sd3539968;
        zetas[45]=-32'sd300467;    zetas[46]=32'sd2348700;    zetas[47]=-32'sd539299;
        zetas[48]=-32'sd1699267;   zetas[49]=-32'sd1643818;   zetas[50]=32'sd3505694;
        zetas[51]=-32'sd3821735;   zetas[52]=32'sd3507263;    zetas[53]=-32'sd2140649;
        zetas[54]=-32'sd1600420;   zetas[55]=32'sd3699596;    zetas[56]=32'sd811944;
        zetas[57]=32'sd531354;     zetas[58]=32'sd954230;     zetas[59]=32'sd3881043;
        zetas[60]=32'sd3900724;    zetas[61]=-32'sd2556880;   zetas[62]=32'sd2071892;
        zetas[63]=-32'sd2797779;   zetas[64]=-32'sd3930395;   zetas[65]=-32'sd1528703;
        zetas[66]=-32'sd3677745;   zetas[67]=-32'sd3041255;   zetas[68]=-32'sd1452451;
        zetas[69]=32'sd3475950;    zetas[70]=32'sd2176455;    zetas[71]=-32'sd1585221;
        zetas[72]=-32'sd1257611;   zetas[73]=32'sd1939314;    zetas[74]=-32'sd4083598;
        zetas[75]=-32'sd1000202;   zetas[76]=-32'sd3190144;   zetas[77]=-32'sd3157330;
        zetas[78]=-32'sd3632928;   zetas[79]=32'sd126922;     zetas[80]=32'sd3412210;
        zetas[81]=-32'sd983419;    zetas[82]=32'sd2147896;    zetas[83]=32'sd2715295;
        zetas[84]=-32'sd2967645;   zetas[85]=-32'sd3693493;   zetas[86]=-32'sd411027;
        zetas[87]=-32'sd2477047;   zetas[88]=-32'sd671102;    zetas[89]=-32'sd1228525;
        zetas[90]=-32'sd22981;     zetas[91]=-32'sd1308169;   zetas[92]=-32'sd381987;
        zetas[93]=32'sd1349076;    zetas[94]=32'sd1852771;    zetas[95]=-32'sd1430430;
        zetas[96]=-32'sd3343383;   zetas[97]=32'sd264944;     zetas[98]=32'sd508951;
        zetas[99]=32'sd3097992;    zetas[100]=32'sd44288;     zetas[101]=-32'sd1100098;
        zetas[102]=32'sd904516;    zetas[103]=32'sd3958618;   zetas[104]=-32'sd3724342;
        zetas[105]=-32'sd8578;     zetas[106]=32'sd1653064;   zetas[107]=-32'sd3249728;
        zetas[108]=32'sd2389356;   zetas[109]=-32'sd210977;   zetas[110]=32'sd759969;
        zetas[111]=-32'sd1316856;  zetas[112]=32'sd189548;    zetas[113]=-32'sd3553272;
        zetas[114]=32'sd3159746;   zetas[115]=-32'sd1851402;  zetas[116]=-32'sd2409325;
        zetas[117]=-32'sd177440;   zetas[118]=32'sd1315589;   zetas[119]=32'sd1341330;
        zetas[120]=32'sd1285669;   zetas[121]=-32'sd1584928;  zetas[122]=-32'sd812732;
        zetas[123]=-32'sd1439742;  zetas[124]=-32'sd3019102;  zetas[125]=-32'sd3881060;
        zetas[126]=-32'sd3628969;  zetas[127]=32'sd3839961;   zetas[128]=32'sd2091667;
        zetas[129]=32'sd3407706;   zetas[130]=32'sd2316500;   zetas[131]=32'sd3817976;
        zetas[132]=-32'sd3342478;  zetas[133]=32'sd2244091;   zetas[134]=-32'sd2446433;
        zetas[135]=-32'sd3562462;  zetas[136]=32'sd266997;    zetas[137]=32'sd2434439;
        zetas[138]=-32'sd1235728;  zetas[139]=32'sd3513181;   zetas[140]=-32'sd3520352;
        zetas[141]=-32'sd3759364;  zetas[142]=-32'sd1197226;  zetas[143]=-32'sd3193378;
        zetas[144]=32'sd900702;    zetas[145]=32'sd1859098;   zetas[146]=32'sd909542;
        zetas[147]=32'sd819034;    zetas[148]=32'sd495491;    zetas[149]=-32'sd1613174;
        zetas[150]=-32'sd43260;    zetas[151]=-32'sd522500;   zetas[152]=-32'sd655327;
        zetas[153]=-32'sd3122442;  zetas[154]=32'sd2031748;   zetas[155]=32'sd3207046;
        zetas[156]=-32'sd3556995;  zetas[157]=-32'sd525098;   zetas[158]=-32'sd768622;
        zetas[159]=-32'sd3595838;  zetas[160]=32'sd342297;    zetas[161]=32'sd286988;
        zetas[162]=-32'sd2437823;  zetas[163]=32'sd4108315;   zetas[164]=32'sd3437287;
        zetas[165]=-32'sd3342277;  zetas[166]=32'sd1735879;   zetas[167]=32'sd203044;
        zetas[168]=32'sd2842341;   zetas[169]=32'sd2691481;   zetas[170]=-32'sd2590150;
        zetas[171]=32'sd1265009;   zetas[172]=32'sd4055324;   zetas[173]=32'sd1247620;
        zetas[174]=32'sd2486353;   zetas[175]=32'sd1595974;   zetas[176]=-32'sd3767016;
        zetas[177]=32'sd1250494;   zetas[178]=32'sd2635921;   zetas[179]=-32'sd3548272;
        zetas[180]=-32'sd2994039;  zetas[181]=32'sd1869119;   zetas[182]=32'sd1903435;
        zetas[183]=-32'sd1050970;  zetas[184]=-32'sd1333058;  zetas[185]=32'sd1237275;
        zetas[186]=-32'sd3318210;  zetas[187]=-32'sd1430225;  zetas[188]=-32'sd451100;
        zetas[189]=32'sd1312455;   zetas[190]=32'sd3306115;   zetas[191]=-32'sd1962642;
        zetas[192]=-32'sd1279661;  zetas[193]=32'sd1917081;   zetas[194]=-32'sd2546312;
        zetas[195]=-32'sd1374803;  zetas[196]=32'sd1500165;   zetas[197]=32'sd777191;
        zetas[198]=32'sd2235880;   zetas[199]=32'sd3406031;   zetas[200]=-32'sd542412;
        zetas[201]=-32'sd2831860;  zetas[202]=-32'sd1671176;  zetas[203]=-32'sd1846953;
        zetas[204]=-32'sd2584293;  zetas[205]=-32'sd3724270;  zetas[206]=32'sd594136;
        zetas[207]=-32'sd3776993;  zetas[208]=-32'sd2013608;  zetas[209]=32'sd2432395;
        zetas[210]=32'sd2454455;   zetas[211]=-32'sd164721;   zetas[212]=32'sd1957272;
        zetas[213]=32'sd3369112;   zetas[214]=32'sd185531;    zetas[215]=-32'sd1207385;
        zetas[216]=-32'sd3183426;  zetas[217]=32'sd162844;    zetas[218]=32'sd1616392;
        zetas[219]=32'sd3014001;   zetas[220]=32'sd810149;    zetas[221]=32'sd1652634;
        zetas[222]=-32'sd3694233;  zetas[223]=-32'sd1799107;  zetas[224]=-32'sd3038916;
        zetas[225]=32'sd3523897;   zetas[226]=32'sd3866901;   zetas[227]=32'sd269760;
        zetas[228]=32'sd2213111;   zetas[229]=-32'sd975884;   zetas[230]=32'sd1717735;
        zetas[231]=32'sd472078;    zetas[232]=-32'sd426683;   zetas[233]=32'sd1723600;
        zetas[234]=-32'sd1803090;  zetas[235]=32'sd1910376;   zetas[236]=-32'sd1667432;
        zetas[237]=-32'sd1104333;  zetas[238]=-32'sd260646;   zetas[239]=-32'sd3833893;
        zetas[240]=-32'sd2939036;  zetas[241]=-32'sd2235985;  zetas[242]=-32'sd420899;
        zetas[243]=-32'sd2286327;  zetas[244]=32'sd183443;    zetas[245]=-32'sd976891;
        zetas[246]=32'sd1612842;   zetas[247]=-32'sd3545687;  zetas[248]=-32'sd554416;
        zetas[249]=32'sd3919660;   zetas[250]=-32'sd48306;    zetas[251]=-32'sd1362209;
        zetas[252]=32'sd3937738;   zetas[253]=32'sd1400424;   zetas[254]=-32'sd846154;
        zetas[255]=32'sd1976782;
    end

    // ---- 状态机的控制寄存器（要在存储例化之前声明：端口 mux 用得到）----
    // S_RUN   每拍发一个蝶形（或一个缩放）
    // S_DRAIN 本层发完，等流水线排空 —— 下一层要读的正是这一层刚写完的那块 buf
    localparam S_IDLE = 2'd0, S_RUN = 2'd1, S_DRAIN = 2'd2, S_DONE = 2'd3;

    reg [1:0] state;
    reg [8:0] len;        // 正变换 128..1，逆变换 1..128
    reg [8:0] grp;        // 当前组的起始下标
    reg [8:0] j;          // 组内偏移
    reg [8:0] k;          // 旋转因子下标
    reg       inv_r;
    reg [8:0] scale_i;
    reg       ph_sc;      // 1 = 正处于逆变换末尾的统一缩放阶段
    reg       sel;        // 哪一块 buf 持有**当前有效**的系数

    wire [8:0] j_hi  = j + len;
    wire       busy_i = (state != S_IDLE);
    wire       issue  = (state == S_RUN);
    wire [7:0] issue_a = ph_sc ? scale_i[7:0] : j[7:0];
    wire [7:0] issue_b = j_hi[7:0];

    // ---- 发射级：把 valid / 写回地址 / 缩放标志打一拍，与 BRAM 的读出对齐 ----
    // BRAM 是同步读：第 T 拍给地址，第 T+1 拍数据才出来。所以送进蝶形的所有伴随
    // 信号都要延后一拍，否则地址会比数据早一拍，写回就错位。
    reg        iv_r, isc_r;
    reg  [7:0] ja_r, jb_r;
    reg signed [31:0] zeta_q;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            iv_r <= 1'b0; isc_r <= 1'b0; ja_r <= 8'd0; jb_r <= 8'd0;
        end else begin
            iv_r  <= issue;
            isc_r <= ph_sc;
            ja_r  <= issue_a;
            jb_r  <= issue_b;
        end
    end

    // 旋转因子 ROM 读出打一拍 —— 原来那条 256:1 组合选择树就在关键路径上。
    // 取负放到 ROM 之后（进蝶形第 1 级寄存器之前），两段各自都短。
    always @(posedge clk) zeta_q <= zetas[k[7:0]];

    // ---- 系数存储：乒乓两块真双口 BRAM ----
    // 忙的时候：buf[sel] 只读（A 口 = j / scale_i，B 口 = j+len），
    //           buf[~sel] 只写（A 口 = j，B 口 = j+len，来自流水线末级）。
    // 空闲的时候：buf[sel] 的 A 口接外部写、B 口接外部读；buf[~sel] 闲置。
    // 读源与写目的分属两块存储，所以层内不存在 RAW，也不需要旁路。
    wire        r0_a_we,  r0_b_we,  r1_a_we,  r1_b_we;
    wire [7:0]  r0_a_addr, r0_b_addr, r1_a_addr, r1_b_addr;
    wire signed [31:0] r0_a_din, r0_b_din, r1_a_din, r1_b_din;
    wire signed [31:0] r0_a_dout, r0_b_dout, r1_a_dout, r1_b_dout;

    ram_dp #(.DW(32), .AW(8)) u_mem0 (
        .clk(clk),
        .a_we(r0_a_we), .a_addr(r0_a_addr), .a_din(r0_a_din), .a_dout(r0_a_dout),
        .b_we(r0_b_we), .b_addr(r0_b_addr), .b_din(r0_b_din), .b_dout(r0_b_dout));
    ram_dp #(.DW(32), .AW(8)) u_mem1 (
        .clk(clk),
        .a_we(r1_a_we), .a_addr(r1_a_addr), .a_din(r1_a_din), .a_dout(r1_a_dout),
        .b_we(r1_b_we), .b_addr(r1_b_addr), .b_din(r1_b_din), .b_dout(r1_b_dout));

    // ---- 流水化的蝶形：CT / GS / 缩放共用一条乘法链 ----
    // tag 里带的是两个写回地址，让地址与数据在同一个模块里同步前进 ——
    // 改流水深度时不可能忘记同步改地址延迟。
    wire signed [31:0] src_a = sel ? r1_a_dout : r0_a_dout;
    wire signed [31:0] src_b = sel ? r1_b_dout : r0_b_dout;

    wire        wb_valid, wb_scale, bf_busy;
    wire [15:0] wb_tag;
    wire signed [31:0] wb_a, wb_b;

    mldsa_butterfly_pipe #(.TAGW(16)) u_bf (
        .clk(clk), .rst_n(rst_n),
        .in_valid(iv_r), .in_tag({ja_r, jb_r}),
        .mode(inv_r), .scale(isc_r),
        .a(src_a), .b(src_b),
        // 缩放阶段送 f = mont²/256；逆变换的蝶形送 −zetas[k]（与组合版同一约定）
        .zeta(isc_r ? FINV : (inv_r ? -zeta_q : zeta_q)),
        .out_valid(wb_valid), .out_tag(wb_tag), .out_scale(wb_scale),
        .a_out(wb_a), .b_out(wb_b), .pipe_busy(bf_busy));

    wire [7:0] wb_ja = wb_tag[15:8];
    wire [7:0] wb_jb = wb_tag[7:0];
    // 发射级也在流水里，排空要连它一起看
    wire       pipe_empty = ~iv_r & ~bf_busy;

    // ---- 端口归属 ----
    // 有效 buf：忙时读、闲时接外部口
    wire       act_a_we   = busy_i ? 1'b0    : wr_en;
    wire [7:0] act_a_addr = busy_i ? issue_a : wr_addr;
    wire [7:0] act_b_addr = busy_i ? issue_b : rd_addr;
    // 目的 buf：流水线末级写回。缩放只写 A 口（B 口没有第二个结果）。
    wire       dst_a_we = wb_valid;
    wire       dst_b_we = wb_valid & ~wb_scale;

    assign r0_a_we   = (sel == 1'b0) ? act_a_we   : dst_a_we;
    assign r0_a_addr = (sel == 1'b0) ? act_a_addr : wb_ja;
    assign r0_a_din  = (sel == 1'b0) ? wr_data    : wb_a;
    assign r0_b_we   = (sel == 1'b0) ? 1'b0       : dst_b_we;
    assign r0_b_addr = (sel == 1'b0) ? act_b_addr : wb_jb;
    assign r0_b_din  = wb_b;

    assign r1_a_we   = (sel == 1'b1) ? act_a_we   : dst_a_we;
    assign r1_a_addr = (sel == 1'b1) ? act_a_addr : wb_ja;
    assign r1_a_din  = (sel == 1'b1) ? wr_data    : wb_a;
    assign r1_b_we   = (sel == 1'b1) ? 1'b0       : dst_b_we;
    assign r1_b_addr = (sel == 1'b1) ? act_b_addr : wb_jb;
    assign r1_b_din  = wb_b;

    // 空闲时 B 口发的就是 rd_addr，所以外部读口直接取有效 buf 的 B 口输出。
    assign rd_data = src_b;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state   <= S_IDLE;
            done    <= 1'b0;
            // 计数器/控制寄存器一律给确定初值：不复位会让复位后头几拍在 4-state
            // 仿真（Icarus）里出现 X。
            len     <= 9'd128;
            grp     <= 9'd0;
            j       <= 9'd0;
            k       <= 9'd1;
            inv_r   <= 1'b0;
            scale_i <= 9'd0;
            ph_sc   <= 1'b0;
            sel     <= 1'b0;
        end else begin
            case (state)
            S_IDLE: begin
                // done 保持到下一次 start，这里不无条件清。
                // 系数写入由上面的端口 mux 直接落到有效 buf 的 A 口，这里不用管。
                if (start) begin
                    done    <= 1'b0;
                    inv_r   <= inverse;
                    len     <= inverse ? 9'd1 : 9'd128;
                    grp     <= 9'd0;
                    j       <= 9'd0;
                    k       <= inverse ? 9'd255 : 9'd1;
                    ph_sc   <= 1'b0;
                    scale_i <= 9'd0;
                    state   <= S_RUN;
                end
            end

            // 每拍发一个。下标推进的逻辑与两拍版逐字相同，只是不再隔一拍。
            S_RUN: begin
                if (ph_sc) begin
                    if (scale_i == 9'd255) state <= S_DRAIN;
                    else                   scale_i <= scale_i + 9'd1;
                end else if (j + 1 < grp + len) begin
                    j <= j + 1;
                end else begin
                    // 一组做完，换旋转因子
                    k <= inv_r ? (k - 9'd1) : (k + 9'd1);
                    if (grp + 2 * len < 9'd256) begin
                        grp <= grp + 2 * len;
                        j   <= grp + 2 * len;
                    end else begin
                        state <= S_DRAIN;   // 一层发完
                    end
                end
            end

            // 排空之后才翻 sel：翻的那一刻起，刚写满的 buf[~sel] 成为新的有效 buf。
            // 必须等空 —— 否则还在飞的写会落到已经改了归属的存储上。
            S_DRAIN: if (pipe_empty) begin
                sel <= ~sel;
                if (ph_sc) begin
                    state <= S_DONE;
                end else if (inv_r) begin
                    if (len == 9'd128) begin
                        ph_sc   <= 1'b1;    // 8 层做完，进统一缩放
                        scale_i <= 9'd0;
                        state   <= S_RUN;
                    end else begin
                        len <= len << 1;
                        grp <= 9'd0;
                        j   <= 9'd0;
                        state <= S_RUN;
                    end
                end else begin
                    if (len == 9'd1) begin
                        state <= S_DONE;
                    end else begin
                        len <= len >> 1;
                        grp <= 9'd0;
                        j   <= 9'd0;
                        state <= S_RUN;
                    end
                end
            end

            S_DONE: begin
                done  <= 1'b1;
                state <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire

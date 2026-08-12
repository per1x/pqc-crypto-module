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
// ⚠️ **读口是同步读，有一拍延迟**：系数存储是一块真双口 BRAM
// （common/ram_dp.v），而 BRAM 没有组合读口。给出 rd_addr 之后要等一个上升沿，
// rd_data 才是那个地址的内容。写口仍是同拍生效。
// 一个蝶形也因此拆成两拍（S_RD 发地址 / S_WB 拿数算完写回），
// 正变换 1025 → 2049 cycles，逆变换 1281 → 2561 cycles。
// 理由与 ML-KEM 侧那一版相同：256×32 的寄存器阵列加多个组合读口，
// 综合出来是几万个 LUT 的选择树，一颗 ZU3EG 放不下 —— 见 docs/fpga-进展.md 的 S3。
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
    localparam S_IDLE  = 3'd0, S_RD    = 3'd1, S_WB   = 3'd2,
               S_SC_RD = 3'd3, S_SC_WB = 3'd4, S_DONE = 3'd5;

    reg [2:0] state;
    reg [8:0] len;        // 正变换 128..1，逆变换 1..128
    reg [8:0] grp;        // 当前组的起始下标
    reg [8:0] j;          // 组内偏移
    reg [8:0] k;          // 旋转因子下标
    reg       inv_r;
    reg [8:0] scale_i;

    wire [8:0] j_hi = j + len;

    // ---- 系数存储：一块真双口 BRAM ----
    // A 口：空闲时接外部写口，蝶形时管 mem[j]，缩放时管 mem[scale_i]；
    // B 口：空闲时接外部读口，蝶形时管 mem[j+len]。
    // 蝶形的两个地址恒不相等（len ≥ 1），不会触发 ram_dp 的同址写断言。
    reg         pa_we,  pb_we;
    reg  [7:0]  pa_addr, pb_addr;
    reg  signed [31:0] pa_din, pb_din;
    wire signed [31:0] pa_dout, pb_dout;

    ram_dp #(.DW(32), .AW(8)) u_mem (
        .clk    (clk),
        .a_we   (pa_we), .a_addr(pa_addr), .a_din(pa_din), .a_dout(pa_dout),
        .b_we   (pb_we), .b_addr(pb_addr), .b_din(pb_din), .b_dout(pb_dout)
    );

    assign rd_data = pb_dout;

    wire signed [31:0] a_val = pa_dout;
    wire signed [31:0] b_val = pb_dout;
    wire signed [31:0] zeta_raw = zetas[k[7:0]];
    // 逆变换用 −zetas[k]
    wire signed [31:0] zeta = inv_r ? -zeta_raw : zeta_raw;

    wire signed [31:0] ct_a, ct_b, gs_a, gs_b;
    mldsa_butterfly_ct u_bf_ct (
        .a(a_val), .b(b_val), .zeta(zeta), .a_out(ct_a), .b_out(ct_b));
    mldsa_butterfly_gs u_bf_gs (
        .a(a_val), .b(b_val), .zeta(zeta), .a_out(gs_a), .b_out(gs_b));

    // S_SC_WB 用：逆变换末尾统一乘 f。缩放只用 A 口，所以取的是 pa_dout。
    wire signed [31:0] scale_in = pa_dout;
    wire signed [63:0] finv_prod =
        $signed({{32{FINV[31]}}, FINV}) * $signed({{32{scale_in[31]}}, scale_in});
    wire signed [31:0] scale_mont;
    mldsa_mont_reduce u_scale_mont (.a(finv_prod), .t_out(scale_mont));

    // ---- 两个 BRAM 口的归属：完全由状态决定 ----
    always @(*) begin
        pa_we   = 1'b0;
        pa_addr = 8'd0;
        pa_din  = 32'sd0;
        pb_we   = 1'b0;
        pb_addr = 8'd0;
        pb_din  = 32'sd0;
        case (state)
        S_IDLE: begin
            pa_we   = wr_en;
            pa_addr = wr_addr;
            pa_din  = wr_data;
            pb_addr = rd_addr;
        end
        S_RD: begin
            pa_addr = j[7:0];
            pb_addr = j_hi[7:0];
        end
        S_WB: begin
            pa_we   = 1'b1;
            pa_addr = j[7:0];
            pa_din  = inv_r ? gs_a : ct_a;
            pb_we   = 1'b1;
            pb_addr = j_hi[7:0];
            pb_din  = inv_r ? gs_b : ct_b;
        end
        S_SC_RD: pa_addr = scale_i[7:0];
        S_SC_WB: begin
            pa_we   = 1'b1;
            pa_addr = scale_i[7:0];
            pa_din  = scale_mont;
        end
        default: ;
        endcase
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state   <= S_IDLE;
            done    <= 1'b0;
            // 数据通路寄存器也给确定初值：不复位会让复位后头几拍在 4-state 仿真
            // （Icarus）里出现 X，也不符合"计数器/控制寄存器要有复位"的通行要求。
            len     <= 9'd128;
            grp     <= 9'd0;
            j       <= 9'd0;
            k       <= 9'd1;
            inv_r   <= 1'b0;
            scale_i <= 9'd0;
        end else begin
            case (state)
            S_IDLE: begin
                // done 保持到下一次 start，这里不无条件清。
                // 系数写入由上面的端口 mux 直接落到 BRAM 的 A 口，这里不用管。
                if (start) begin
                    done  <= 1'b0;
                    inv_r <= inverse;
                    len   <= inverse ? 9'd1 : 9'd128;
                    grp   <= 9'd0;
                    j     <= 9'd0;
                    k     <= inverse ? 9'd255 : 9'd1;
                    state <= S_RD;
                end
            end

            // 第一拍：地址已经由 mux 发给 BRAM，等一个沿把 mem[j] / mem[j+len]
            // 装进输出寄存器，下一拍才能用。
            S_RD: state <= S_WB;

            S_WB: begin
                // 第二拍：蝶形结果由 mux 写回两个地址，这里只推进下标。
                state <= S_RD;

                if (j + 1 < grp + len) begin
                    j <= j + 1;
                end else begin
                    // 一组做完，换旋转因子
                    k <= inv_r ? (k - 9'd1) : (k + 9'd1);
                    if (grp + 2 * len < 9'd256) begin
                        grp <= grp + 2 * len;
                        j   <= grp + 2 * len;
                    end else begin
                        // 一层做完
                        if (inv_r) begin
                            if (len == 9'd128) begin
                                scale_i <= 9'd0;
                                state   <= S_SC_RD;
                            end else begin
                                len <= len << 1;
                                grp <= 9'd0;
                                j   <= 9'd0;
                            end
                        end else begin
                            if (len == 9'd1) begin
                                state <= S_DONE;
                            end else begin
                                len <= len >> 1;
                                grp <= 9'd0;
                                j   <= 9'd0;
                            end
                        end
                    end
                end
            end

            // 缩放也是两拍：读一个系数，乘完写回同一地址。
            S_SC_RD: state <= S_SC_WB;

            S_SC_WB: begin
                // 写回由端口 mux 完成，这里只推进下标。
                if (scale_i == 9'd255) begin
                    state <= S_DONE;
                end else begin
                    scale_i <= scale_i + 9'd1;
                    state   <= S_SC_RD;
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

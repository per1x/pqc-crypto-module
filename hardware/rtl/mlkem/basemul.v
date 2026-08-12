// mlkem_basemul —— NTT 域的基乘（FIPS 203, q = 3329）
//
// ML-KEM 的 NTT 只做 7 层，变换结果不是 256 个标量，而是 128 个一次多项式，
// 每一对系数落在 Z_q[x]/(x² − ζ) 里。所以 NTT 域的"逐点乘"实际是：
//
//     (a0 + a1·x)(b0 + b1·x) mod (x² − ζ)
//       = (a0·b0 + a1·b1·ζ) + (a0·b1 + a1·b0)·x
//
// 所有乘法都在 Montgomery 域完成，与 mont_reduce.v 的约定一致：
// fqmul(a, b) = mont(a·b) ≡ a·b·2⁻¹⁶ (mod q)。因此
//
//     r0 = fqmul(fqmul(a1, b1), ζ) + fqmul(a0, b0)
//     r1 = fqmul(a0, b1)           + fqmul(a1, b0)
//
// a1·b1 先约减再乘 ζ，是为了把中间结果压回 (−q, q)，
// 保证第二次 mont_reduce 的输入仍满足 |a| < q·2¹⁵。
//
// 一对系数用 +ζ，相邻一对用 −ζ（x² − ζ 与 x² + ζ 交替），
// 符号由调用方在 zeta 端口上给出，本模块不做区分。
//
// 纯组合逻辑，五个乘法器 —— 真实的核里这五次乘法会分时复用同一块 DSP。
//
// 【为什么拆成 _head / _tail 两半】
// r0 这条路上串着 6 次乘法：smul(a1,b1) 一次、它的 mont_reduce 两次
// （m = a·QINV、m·Q）、smul(·,ζ) 一次、再一次 mont_reduce 两次。
// 在 ZU3EG 上一级 DSP 乘加大约 2.4 ns，6 级就是 14 ns 上下 —— 再接上调用方
// 的 barrett（又是 2 次乘法）直接超过 20 ns，**100 MHz 收不住**。
// 实测 mlkem_keygen 第一版 WNS −10.722 ns，关键路径正是这一条。
//
// 所以把它按 t_a1b1 切成两半，让**需要跑 100 MHz 的核**（mlkem_keygen）
// 在中间插一级寄存器；组合版 mlkem_basemul 仍然由这两半拼出来，
// 接口和原有的 cocotb 用例都不变 —— 同一段数学只有一份实现。
`default_nettype none

// 前半：a1·b1 的那一次 fqmul。切在这里是因为它正好把 6 级乘法分成 3 + 3。
module mlkem_basemul_head (
    input  wire signed [15:0] a1,
    input  wire signed [15:0] b1,
    output wire signed [15:0] t_a1b1
);
    mont_reduce u_a1b1 (
        .a($signed({{16{a1[15]}}, a1}) * $signed({{16{b1[15]}}, b1})),
        .t_out(t_a1b1));
endmodule

// 后半：剩下的四次 fqmul 与两个加法。四次之间互相独立，深度仍是 3 级乘法。
module mlkem_basemul_tail (
    input  wire signed [15:0] a0,
    input  wire signed [15:0] a1,
    input  wire signed [15:0] b0,
    input  wire signed [15:0] b1,
    input  wire signed [15:0] zeta,
    input  wire signed [15:0] t_a1b1,
    output wire signed [15:0] r0,
    output wire signed [15:0] r1
);
    function automatic signed [31:0] smul;
        input signed [15:0] x;
        input signed [15:0] y;
        begin
            smul = $signed({{16{x[15]}}, x}) * $signed({{16{y[15]}}, y});
        end
    endfunction

    wire signed [15:0] t_zeta, t_a0b0, t_a0b1, t_a1b0;
    mont_reduce u_zeta (.a(smul(t_a1b1, zeta)), .t_out(t_zeta));
    mont_reduce u_a0b0 (.a(smul(a0, b0)),       .t_out(t_a0b0));
    mont_reduce u_a0b1 (.a(smul(a0, b1)),       .t_out(t_a0b1));
    mont_reduce u_a1b0 (.a(smul(a1, b0)),       .t_out(t_a1b0));

    assign r0 = t_zeta + t_a0b0;
    assign r1 = t_a0b1 + t_a1b0;
endmodule

// 基乘的 ζ 取值 —— 表就是 ZETAS[64..127]，也就是 7 层 NTT 之后剩下的那一层。
//
// 【为什么单独成模块】
// 调用基乘的核（keygen / encaps / decaps）都要这张表，而且取法完全一样：
// 第 pair 对系数用 bz[pair>>1]，奇数对取负号（x² − ζ 与 x² + ζ 交替）。
// 抄三份的话，改一个数就得记得改三处 —— 这类"数据副本"正是最容易改漏的地方。
// 所以表和取法一起放在这里，各核只例化。
//
// 综合出来是 64×16 的分布式 ROM，几十个 LUT；三个核各例化一份仍然比
// 共享一份再做仲裁便宜，共用的是**源码**而不是硅上的那块 ROM。
module mlkem_bmzeta (
    input  wire        [7:0]  pair,     // 0..127：偶数用 +ζ，奇数用 −ζ
    output wire signed [15:0] zeta
);
    (* rom_style = "distributed" *)
    reg signed [15:0] bz [0:63];
    initial begin
        bz[ 0]=-16'sd1103; bz[ 1]= 16'sd430;  bz[ 2]= 16'sd555;  bz[ 3]= 16'sd843;
        bz[ 4]=-16'sd1251; bz[ 5]= 16'sd871;  bz[ 6]= 16'sd1550; bz[ 7]= 16'sd105;
        bz[ 8]= 16'sd422;  bz[ 9]= 16'sd587;  bz[10]= 16'sd177;  bz[11]=-16'sd235;
        bz[12]=-16'sd291;  bz[13]=-16'sd460;  bz[14]= 16'sd1574; bz[15]= 16'sd1653;
        bz[16]=-16'sd246;  bz[17]= 16'sd778;  bz[18]= 16'sd1159; bz[19]=-16'sd147;
        bz[20]=-16'sd777;  bz[21]= 16'sd1483; bz[22]=-16'sd602;  bz[23]= 16'sd1119;
        bz[24]=-16'sd1590; bz[25]= 16'sd644;  bz[26]=-16'sd872;  bz[27]= 16'sd349;
        bz[28]= 16'sd418;  bz[29]= 16'sd329;  bz[30]=-16'sd156;  bz[31]=-16'sd75;
        bz[32]= 16'sd817;  bz[33]= 16'sd1097; bz[34]= 16'sd603;  bz[35]= 16'sd610;
        bz[36]= 16'sd1322; bz[37]=-16'sd1285; bz[38]=-16'sd1465; bz[39]= 16'sd384;
        bz[40]=-16'sd1215; bz[41]=-16'sd136;  bz[42]= 16'sd1218; bz[43]=-16'sd1335;
        bz[44]=-16'sd874;  bz[45]= 16'sd220;  bz[46]=-16'sd1187; bz[47]=-16'sd1659;
        bz[48]=-16'sd1185; bz[49]=-16'sd1530; bz[50]=-16'sd1278; bz[51]= 16'sd794;
        bz[52]=-16'sd1510; bz[53]=-16'sd854;  bz[54]=-16'sd870;  bz[55]= 16'sd478;
        bz[56]=-16'sd108;  bz[57]=-16'sd308;  bz[58]= 16'sd996;  bz[59]= 16'sd991;
        bz[60]= 16'sd958;  bz[61]=-16'sd1460; bz[62]= 16'sd1522; bz[63]= 16'sd1628;
    end

    wire signed [15:0] raw = bz[pair[7:1]];
    assign zeta = pair[0] ? -raw : raw;
endmodule

module mlkem_basemul (
    input  wire signed [15:0] a0,
    input  wire signed [15:0] a1,
    input  wire signed [15:0] b0,
    input  wire signed [15:0] b1,
    input  wire signed [15:0] zeta,
    output wire signed [15:0] r0,
    output wire signed [15:0] r1
);

    // 组合版 = 前半 + 后半直接串起来，不插寄存器。
    // 保留它是因为它是"基乘"这件事最直白的写法，也是 cocotb 组合用例的被测对象。
    wire signed [15:0] t_a1b1;
    mlkem_basemul_head u_head (.a1(a1), .b1(b1), .t_a1b1(t_a1b1));
    mlkem_basemul_tail u_tail (
        .a0(a0), .a1(a1), .b0(b0), .b1(b1), .zeta(zeta),
        .t_a1b1(t_a1b1), .r0(r0), .r1(r1));
endmodule

`default_nettype wire

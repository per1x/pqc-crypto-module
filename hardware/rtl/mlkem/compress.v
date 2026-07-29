// mlkem_compress / mlkem_decompress —— FIPS 203 的系数压缩与解压
//
// 定义（FIPS 203 §4.2.1）：
//     Compress_d(x)   = round(2^d / q · x)  mod 2^d
//     Decompress_d(y) = round(q / 2^d · y)
// 其中 round 取"四舍五入、半值向上"。
//
// 【压缩里的除法怎么变成硬件】
// 定义式展开成整数运算是 floor(((x << d) + q/2) / q)，分母是常数 q = 3329。
// 硬件里不做真除法，改成"乘倒数再右移"：
//
//     floor(N / q) == (N · RECIP) >> 33,   RECIP = ceil(2^33 / q) = 2580335
//
// 这个等式对本模块可能出现的全部 N（0 ≤ N ≤ (q−1)·2^11 + q/2 = 6817408）成立，
// 由 hardware/tb/cocotb/test_compress.py 在整个输入域上逐值验证 ——
// 输入只有 q = 3329 种取值，所以这里的覆盖是穷举而不是抽样。
//
// 【负系数的处理】
// 数据通路里的系数用 (−q, q) 的有符号表示，压缩前要先折回 [0, q)：
// 负数加一个 q。这一步与参考实现的 `u += (u >> 15) & q` 等价。
//
// 【解压不需要倒数】
// Decompress 的除数是 2^d，右移即可，结果精确。
//
// D 由参数给出：ML-KEM 用到 D ∈ {1, 4, 5, 10, 11}。
`default_nettype none

module mlkem_compress #(
    parameter integer D = 10
) (
    input  wire signed [15:0] coeff,
    output wire       [D-1:0] val
);
    localparam [21:0] RECIP = 22'd2580335;   // ceil(2^33 / q)

    // 折回 [0, q)
    wire signed [15:0] norm = coeff[15] ? (coeff + 16'sd3329) : coeff;
    wire        [11:0] u    = norm[11:0];

    // N = (u << D) + q/2；u 最多 12 位，D 最多 11，加常数后仍在 24 位内
    wire [23:0] num = ({12'd0, u} << D) + 24'd1664;

    // 位宽显式对齐后再乘，避免依赖上下文位宽推断
    wire [45:0] num_ext   = {22'd0, num};
    wire [45:0] recip_ext = {24'd0, RECIP};
    wire [45:0] prod      = num_ext * recip_ext;

    wire [12:0] quot = prod[45:33];

    // mod 2^D 就是取低 D 位
    assign val = quot[D-1:0];
endmodule

module mlkem_decompress #(
    parameter integer D = 10
) (
    input  wire        [D-1:0] val,
    output wire signed [15:0]  coeff
);
    // num = q·y + 2^(D−1)，最大 (2^11−1)·q + 2^10 < 2^23
    wire [26:0] val_ext = {{(27 - D){1'b0}}, val};
    wire [26:0] num     = 27'd3329 * val_ext + (27'd1 << (D - 1));

    // 右移 D 位即为结果，落在 [0, q)，用 12 位承接
    assign coeff = {4'd0, num[D + 11:D]};
endmodule

`default_nettype wire

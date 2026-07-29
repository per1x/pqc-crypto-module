// mlkem_encode12 / mlkem_decode12 —— 12 位系数与字节流之间的转换
//
// FIPS 203 的 ByteEncode_d 把每个系数的 d 个比特按低位在前依次接进比特流，
// 字节也按低位在前组装。对 d < 12 的压缩系数（已经落在 [0, 2^d)）这一步
// 没有任何逻辑，纯粹是导线：把 8 个 d 位系数拼起来正好是 d 个字节。
// 因此只有 12 位这一路值得做成模块 —— 它多一步"把有符号系数折回 [0, q)"，
// 那才是真正的组合逻辑。
//
// 编码：两个系数打进 3 个字节
//     byte0 = t0[7:0]
//     byte1 = t0[11:8] | t1[3:0] << 4
//     byte2 = t1[11:4]
// 解码是它的逆。解码不做范围检查：FIPS 203 的 ByteDecode12 允许 [0, 2^12)
// 的取值，越界的处理属于上层协议（ML-KEM 的密钥合法性检查）。
`default_nettype none

module mlkem_encode12 (
    input  wire signed [15:0] c0,
    input  wire signed [15:0] c1,
    output wire        [23:0] bytes_out   // bytes_out[7:0] 是最低地址那一字节
);
    // 折回 [0, q)：数据通路里的系数用 (−q, q) 的有符号表示
    wire signed [15:0] n0 = c0[15] ? (c0 + 16'sd3329) : c0;
    wire signed [15:0] n1 = c1[15] ? (c1 + 16'sd3329) : c1;

    wire [11:0] t0 = n0[11:0];
    wire [11:0] t1 = n1[11:0];

    assign bytes_out = {t1[11:4], t1[3:0], t0[11:8], t0[7:0]};
endmodule

module mlkem_decode12 (
    input  wire [23:0] bytes_in,
    output wire [11:0] c0,
    output wire [11:0] c1
);
    assign c0 = {bytes_in[11:8],  bytes_in[7:0]};
    assign c1 = {bytes_in[23:16], bytes_in[15:12]};
endmodule

`default_nettype wire

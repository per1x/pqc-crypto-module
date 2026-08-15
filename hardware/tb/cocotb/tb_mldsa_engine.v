// 把 mldsa_engine 接上一个 sha3_core，供 cocotb 当顶层用
//
// engine 只有**一个字节写口**，所以这个测试台不需要像 tb_mldsa_keygen/sign/verify
// 那样把三个核各自的原生口引出来 —— 用例只经 engine 的对外契约说话，
// 与 mldsa_axi 看到的是同一副面孔。这正是要验的东西：
// **字节流排布 → 三个核原生端口** 这一层翻译对不对。
//
// 海绵接在外面（engine 不自带），与 mldsa_axi 的接法一致：
// engine 的 sha_* 口直连 sha3_core，zeroize 也一并接真。
`default_nettype none
// engine 已经是**运行时选参数集**的，所以这个测试台不再需要任何参数 ——
// 参数集由用例通过 pset 端口在运行时给，甚至可以在同一次仿真里换。
module tb_mldsa_engine (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        zeroize,
    output wire        wiping,

    input  wire        start,
    input  wire [1:0]  op,
    input  wire [1:0]  pset,
    output wire        busy,
    output wire        done,
    output wire        verify_ok,

    input  wire        in_we,
    input  wire [14:0] in_addr,
    input  wire [7:0]  in_data,
    input  wire [15:0] msg_len,
    input  wire [15:0] ctx_len,

    input  wire [14:0] out_addr,
    output wire [7:0]  out_data,
    output wire [15:0] out_len
);
    wire        ss, siv, sif, sor;
    wire [7:0]  sr, su, sid, sod;
    wire        sir, sov;

    mldsa_engine u_eng (
        .clk(clk), .rst_n(rst_n),
        .zeroize(zeroize), .wiping(wiping),
        .start(start), .op(op), .pset(pset),
        .busy(busy), .done(done), .verify_ok(verify_ok),
        .in_we(in_we), .in_addr(in_addr), .in_data(in_data),
        .msg_len(msg_len), .ctx_len(ctx_len),
        .out_addr(out_addr), .out_data(out_data), .out_len(out_len),
        .sha_start(ss), .sha_rate(sr), .sha_suffix(su),
        .sha_in_valid(siv), .sha_in_data(sid), .sha_in_flush(sif),
        .sha_in_ready(sir), .sha_out_valid(sov), .sha_out_data(sod),
        .sha_out_ready(sor));

    sha3_core u_sha (
        .clk(clk), .rst_n(rst_n), .rate_bytes(sr), .suffix(su),
        .start(ss), .zeroize(zeroize),
        .in_valid(siv), .in_ready(sir), .in_data(sid), .in_flush(sif),
        .out_valid(sov), .out_ready(sor), .out_data(sod),
        .busy(), .absorbing(), .squeezing(),
        .ext_start(1'b0), .ext_done(), .ext_wr_en(1'b0),
        .ext_wr_addr(5'd0), .ext_wr_data(64'd0), .ext_rd_addr(5'd0), .ext_rd_data());
endmodule
`default_nettype wire

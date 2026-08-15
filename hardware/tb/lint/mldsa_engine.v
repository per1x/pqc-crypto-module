// mldsa_engine —— **lint 专用空壳**，只有端口表，没有任何行为
//
// ============================================================================
// 【为什么需要它，以及它什么时候自己消失】
// ============================================================================
// mldsa_axi 例化 mldsa_engine，而真正的 engine（NTT 流水化 + 时分复用 +
// 运行时选参数集）在另一条线上做，还没落进 hardware/rtl/mldsa/。
// 而 Verilator 是**把命令行上所有文件一起看**的：少一个模块就 MODMISSING，
// 那会让**整仓 lint 全红**（每个模块都失败），等于没有 lint ——
// （⚠️ 顺带记一条：注释行**不能以 "// Verilator" 开头** —— Verilator 会把它
//   当成 `// verilator` 元注释去解析，报一个位置对不上的语法错误。
//   这个空壳第一版就是这么写的，全仓 lint 当场全红。）
// 与 vendor_stubs.v 存在的理由完全一样，见那个文件头。
//
// tools/rtl_lint.sh 里只在 hardware/rtl/mldsa/mldsa_engine.v **不存在**时才把
// 本文件加进去。真 engine 一落地，这个空壳自动不再参与，不需要谁记得删。
//
// ⚠️ 它**只进 lint**：不进 cocotb（那边用 hardware/tb/cocotb/stub_mldsa_engine.v
//    那个有行为的替身），更不进 impl_bitstream.tcl 的文件清单
//    —— 真被综合到的话，出来的 bitstream 里 ML-DSA 是个空壳。
//
// 端口表照着双方约定的那一份抄全：端口名/位宽写错了 lint 会当场报，
// 也就是说这个空壳顺带在替两条线校验接口的拼写。
`default_nettype none

module mldsa_engine (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        start,          // 脉冲
    input  wire [1:0]  op,             // 0=KeyGen 1=Sign 2=Verify
    input  wire [1:0]  pset,           // 0=44 1=65 2=87
    output wire        busy,
    output wire        done,
    output wire        verify_ok,      // op=Verify 时有效

    input  wire        in_we,
    input  wire [14:0] in_addr,
    input  wire [7:0]  in_data,
    input  wire [15:0] msg_len,
    input  wire [15:0] ctx_len,

    input  wire [14:0] out_addr,
    output wire [7:0]  out_data,
    output wire [15:0] out_len,

    output wire        sha_start,
    output wire [7:0]  sha_rate,
    output wire [7:0]  sha_suffix,
    output wire        sha_in_valid,
    output wire [7:0]  sha_in_data,
    output wire        sha_in_flush,
    input  wire        sha_in_ready,
    input  wire        sha_out_valid,
    input  wire [7:0]  sha_out_data,
    output wire        sha_out_ready
);
    // 全部输出恒零：这个文件不做任何事，只让 lint 有个模块可查。
    assign busy = 1'b0;   assign done = 1'b0;   assign verify_ok = 1'b0;
    assign out_data = 8'd0; assign out_len = 16'd0;
    assign sha_start = 1'b0; assign sha_rate = 8'd0; assign sha_suffix = 8'd0;
    assign sha_in_valid = 1'b0; assign sha_in_data = 8'd0;
    assign sha_in_flush = 1'b0; assign sha_out_ready = 1'b0;

    wire _unused = &{1'b0, clk, rst_n, start, op, pset,
                     in_we, in_addr, in_data, msg_len, ctx_len, out_addr,
                     sha_in_ready, sha_out_valid, sha_out_data, 1'b0};

endmodule

`default_nettype wire

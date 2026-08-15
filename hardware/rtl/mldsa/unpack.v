// ML-DSA 的通用位解包（mldsa_bitpack 的逆）—— skDecode / ExpandMask 用
//
// ============================================================================
// 【这一层解决什么】
// ============================================================================
// 打包时每个系数 W 位、低位在前、攒够 8 位吐一字节（见 pack.v）。解包要反着来：
// **字节一个个进来，W 位系数一个个出去**，内部一个位累加器。位宽不是 8 的因数
// （3/13/18 位）时每个系数横跨相邻字节，这个累加器把「跨字节」这件麻烦事收在一处。
//
// 与 mldsa_bitpack 完全对偶：
//   · 累加器低位先用（先进的字节在低位，先出的系数取自低 W 位）；
//   · nbits < W 时才吸字节（in_ready），nbits ≥ W 时才吐系数（out_valid）——
//     两者互斥，一拍只做一件事。
//
// ⚠️ 这里只做「位」的搬运，**不含 η−v / 2^(D−1)−v / γ₁−v 那类逆变换**。
// 那些变换是有符号/无符号搬移，放在调用方（sign.v）做，理由同 pack.v：
// 让本模块的正确性只依赖「宽度对不对」一件事。
`default_nettype none

module mldsa_bitunpack #(
    parameter integer W = 13          // 每个系数占的位数
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clr,           // 每条多项式开始前拉一拍，清空累加器

    input  wire [7:0]  in_byte,
    input  wire        in_valid,
    output wire        in_ready,      // 累加器不足 W 位时才收字节

    output wire [W-1:0] out_val,      // 低 W 位就是下一个系数的原始无符号值
    output wire         out_valid,    // 累加器已够 W 位
    input  wire         out_ready
);
    // 累加器要装得下「吐一个系数前的残留（最多 W−1 位）」+「新吸的一整字节（8 位）」。
    // W−1+8 = W+7，取 W+8 留一位余量，与 mldsa_bitpack 对称。
    reg [W+8-1:0] acc;
    reg [4:0]     nbits;

    assign out_valid = (nbits >= W[4:0]);
    assign in_ready  = (nbits <  W[4:0]);
    assign out_val   = acc[W-1:0];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            acc <= {(W+8){1'b0}};
            nbits <= 5'd0;
        end else if (clr) begin
            acc <= {(W+8){1'b0}};
            nbits <= 5'd0;
        end else if (out_valid && out_ready) begin
            // 吐掉低 W 位，剩下的下移
            acc   <= acc >> W;
            nbits <= nbits - W[4:0];
        end else if (in_valid && in_ready) begin
            // 新字节拼到高位侧：低位在前的约定就是这一句（与 bitpack 对偶）
            acc   <= acc | ({{(W){1'b0}}, in_byte} << nbits);
            nbits <= nbits + 5'd8;
        end
    end
endmodule

`default_nettype wire

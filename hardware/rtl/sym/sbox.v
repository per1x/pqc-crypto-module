// sbox.v —— AES 与 SM4 的 S 盒（**每张表在本仓库里只有这一份**）
//
// 表是 hardware/model/sym_oracle.py 用 `--emit-sbox` 生成的：
//
//     python3 hardware/model/sym_oracle.py --emit-sbox
//
// **RTL 里放表、模型里放代数定义**，这样分工是有意的：
//   · AES 的 S 盒在 sym_oracle.py 里是**从 GF(2^8) 求逆 + 仿射变换算出来的**，
//     不是抄的表。所以"两边抄错同一格"这件事不可能发生。
//   · 测试台 test_aes 把 256 个输入**逐值**过一遍两张表，与模型算出来的比对。
//     表错一格是必然被抓住，不是可能被抓住。
//   · SM4 的 S 盒标准正文只给表（代数参数要反推），所以模型那边也是表；
//     它的正确性由 GB/T 32907 附录 A 的密文端到端保证 —— 错一格，密文就不对。
//
// 硬件里的查表是**天然常量时间**的：没有 cache、没有 TLB，任何输入都是同一条
// 组合路径。软件 AES 那些 T-table / cache 时序攻击在这里不存在 ——
// 但这是"这类攻击面不存在"，不是"我们做了防护"，别把两件事说成一件。
`default_nettype none

// AES 的 S 盒（FIPS 197 §5.1.1）
module aes_sbox (
    input  wire [7:0] x,
    output reg  [7:0] y
);
    always @(*) begin
        case (x)
        8'h00: y = 8'h63; 8'h01: y = 8'h7c; 8'h02: y = 8'h77; 8'h03: y = 8'h7b; 8'h04: y = 8'hf2; 8'h05: y = 8'h6b; 8'h06: y = 8'h6f; 8'h07: y = 8'hc5;
        8'h08: y = 8'h30; 8'h09: y = 8'h01; 8'h0a: y = 8'h67; 8'h0b: y = 8'h2b; 8'h0c: y = 8'hfe; 8'h0d: y = 8'hd7; 8'h0e: y = 8'hab; 8'h0f: y = 8'h76;
        8'h10: y = 8'hca; 8'h11: y = 8'h82; 8'h12: y = 8'hc9; 8'h13: y = 8'h7d; 8'h14: y = 8'hfa; 8'h15: y = 8'h59; 8'h16: y = 8'h47; 8'h17: y = 8'hf0;
        8'h18: y = 8'had; 8'h19: y = 8'hd4; 8'h1a: y = 8'ha2; 8'h1b: y = 8'haf; 8'h1c: y = 8'h9c; 8'h1d: y = 8'ha4; 8'h1e: y = 8'h72; 8'h1f: y = 8'hc0;
        8'h20: y = 8'hb7; 8'h21: y = 8'hfd; 8'h22: y = 8'h93; 8'h23: y = 8'h26; 8'h24: y = 8'h36; 8'h25: y = 8'h3f; 8'h26: y = 8'hf7; 8'h27: y = 8'hcc;
        8'h28: y = 8'h34; 8'h29: y = 8'ha5; 8'h2a: y = 8'he5; 8'h2b: y = 8'hf1; 8'h2c: y = 8'h71; 8'h2d: y = 8'hd8; 8'h2e: y = 8'h31; 8'h2f: y = 8'h15;
        8'h30: y = 8'h04; 8'h31: y = 8'hc7; 8'h32: y = 8'h23; 8'h33: y = 8'hc3; 8'h34: y = 8'h18; 8'h35: y = 8'h96; 8'h36: y = 8'h05; 8'h37: y = 8'h9a;
        8'h38: y = 8'h07; 8'h39: y = 8'h12; 8'h3a: y = 8'h80; 8'h3b: y = 8'he2; 8'h3c: y = 8'heb; 8'h3d: y = 8'h27; 8'h3e: y = 8'hb2; 8'h3f: y = 8'h75;
        8'h40: y = 8'h09; 8'h41: y = 8'h83; 8'h42: y = 8'h2c; 8'h43: y = 8'h1a; 8'h44: y = 8'h1b; 8'h45: y = 8'h6e; 8'h46: y = 8'h5a; 8'h47: y = 8'ha0;
        8'h48: y = 8'h52; 8'h49: y = 8'h3b; 8'h4a: y = 8'hd6; 8'h4b: y = 8'hb3; 8'h4c: y = 8'h29; 8'h4d: y = 8'he3; 8'h4e: y = 8'h2f; 8'h4f: y = 8'h84;
        8'h50: y = 8'h53; 8'h51: y = 8'hd1; 8'h52: y = 8'h00; 8'h53: y = 8'hed; 8'h54: y = 8'h20; 8'h55: y = 8'hfc; 8'h56: y = 8'hb1; 8'h57: y = 8'h5b;
        8'h58: y = 8'h6a; 8'h59: y = 8'hcb; 8'h5a: y = 8'hbe; 8'h5b: y = 8'h39; 8'h5c: y = 8'h4a; 8'h5d: y = 8'h4c; 8'h5e: y = 8'h58; 8'h5f: y = 8'hcf;
        8'h60: y = 8'hd0; 8'h61: y = 8'hef; 8'h62: y = 8'haa; 8'h63: y = 8'hfb; 8'h64: y = 8'h43; 8'h65: y = 8'h4d; 8'h66: y = 8'h33; 8'h67: y = 8'h85;
        8'h68: y = 8'h45; 8'h69: y = 8'hf9; 8'h6a: y = 8'h02; 8'h6b: y = 8'h7f; 8'h6c: y = 8'h50; 8'h6d: y = 8'h3c; 8'h6e: y = 8'h9f; 8'h6f: y = 8'ha8;
        8'h70: y = 8'h51; 8'h71: y = 8'ha3; 8'h72: y = 8'h40; 8'h73: y = 8'h8f; 8'h74: y = 8'h92; 8'h75: y = 8'h9d; 8'h76: y = 8'h38; 8'h77: y = 8'hf5;
        8'h78: y = 8'hbc; 8'h79: y = 8'hb6; 8'h7a: y = 8'hda; 8'h7b: y = 8'h21; 8'h7c: y = 8'h10; 8'h7d: y = 8'hff; 8'h7e: y = 8'hf3; 8'h7f: y = 8'hd2;
        8'h80: y = 8'hcd; 8'h81: y = 8'h0c; 8'h82: y = 8'h13; 8'h83: y = 8'hec; 8'h84: y = 8'h5f; 8'h85: y = 8'h97; 8'h86: y = 8'h44; 8'h87: y = 8'h17;
        8'h88: y = 8'hc4; 8'h89: y = 8'ha7; 8'h8a: y = 8'h7e; 8'h8b: y = 8'h3d; 8'h8c: y = 8'h64; 8'h8d: y = 8'h5d; 8'h8e: y = 8'h19; 8'h8f: y = 8'h73;
        8'h90: y = 8'h60; 8'h91: y = 8'h81; 8'h92: y = 8'h4f; 8'h93: y = 8'hdc; 8'h94: y = 8'h22; 8'h95: y = 8'h2a; 8'h96: y = 8'h90; 8'h97: y = 8'h88;
        8'h98: y = 8'h46; 8'h99: y = 8'hee; 8'h9a: y = 8'hb8; 8'h9b: y = 8'h14; 8'h9c: y = 8'hde; 8'h9d: y = 8'h5e; 8'h9e: y = 8'h0b; 8'h9f: y = 8'hdb;
        8'ha0: y = 8'he0; 8'ha1: y = 8'h32; 8'ha2: y = 8'h3a; 8'ha3: y = 8'h0a; 8'ha4: y = 8'h49; 8'ha5: y = 8'h06; 8'ha6: y = 8'h24; 8'ha7: y = 8'h5c;
        8'ha8: y = 8'hc2; 8'ha9: y = 8'hd3; 8'haa: y = 8'hac; 8'hab: y = 8'h62; 8'hac: y = 8'h91; 8'had: y = 8'h95; 8'hae: y = 8'he4; 8'haf: y = 8'h79;
        8'hb0: y = 8'he7; 8'hb1: y = 8'hc8; 8'hb2: y = 8'h37; 8'hb3: y = 8'h6d; 8'hb4: y = 8'h8d; 8'hb5: y = 8'hd5; 8'hb6: y = 8'h4e; 8'hb7: y = 8'ha9;
        8'hb8: y = 8'h6c; 8'hb9: y = 8'h56; 8'hba: y = 8'hf4; 8'hbb: y = 8'hea; 8'hbc: y = 8'h65; 8'hbd: y = 8'h7a; 8'hbe: y = 8'hae; 8'hbf: y = 8'h08;
        8'hc0: y = 8'hba; 8'hc1: y = 8'h78; 8'hc2: y = 8'h25; 8'hc3: y = 8'h2e; 8'hc4: y = 8'h1c; 8'hc5: y = 8'ha6; 8'hc6: y = 8'hb4; 8'hc7: y = 8'hc6;
        8'hc8: y = 8'he8; 8'hc9: y = 8'hdd; 8'hca: y = 8'h74; 8'hcb: y = 8'h1f; 8'hcc: y = 8'h4b; 8'hcd: y = 8'hbd; 8'hce: y = 8'h8b; 8'hcf: y = 8'h8a;
        8'hd0: y = 8'h70; 8'hd1: y = 8'h3e; 8'hd2: y = 8'hb5; 8'hd3: y = 8'h66; 8'hd4: y = 8'h48; 8'hd5: y = 8'h03; 8'hd6: y = 8'hf6; 8'hd7: y = 8'h0e;
        8'hd8: y = 8'h61; 8'hd9: y = 8'h35; 8'hda: y = 8'h57; 8'hdb: y = 8'hb9; 8'hdc: y = 8'h86; 8'hdd: y = 8'hc1; 8'hde: y = 8'h1d; 8'hdf: y = 8'h9e;
        8'he0: y = 8'he1; 8'he1: y = 8'hf8; 8'he2: y = 8'h98; 8'he3: y = 8'h11; 8'he4: y = 8'h69; 8'he5: y = 8'hd9; 8'he6: y = 8'h8e; 8'he7: y = 8'h94;
        8'he8: y = 8'h9b; 8'he9: y = 8'h1e; 8'hea: y = 8'h87; 8'heb: y = 8'he9; 8'hec: y = 8'hce; 8'hed: y = 8'h55; 8'hee: y = 8'h28; 8'hef: y = 8'hdf;
        8'hf0: y = 8'h8c; 8'hf1: y = 8'ha1; 8'hf2: y = 8'h89; 8'hf3: y = 8'h0d; 8'hf4: y = 8'hbf; 8'hf5: y = 8'he6; 8'hf6: y = 8'h42; 8'hf7: y = 8'h68;
        8'hf8: y = 8'h41; 8'hf9: y = 8'h99; 8'hfa: y = 8'h2d; 8'hfb: y = 8'h0f; 8'hfc: y = 8'hb0; 8'hfd: y = 8'h54; 8'hfe: y = 8'hbb; 8'hff: y = 8'h16;
        default: y = 8'h00;
        endcase
    end
endmodule

// AES 的逆 S 盒（FIPS 197 §5.3.2）
module aes_inv_sbox (
    input  wire [7:0] x,
    output reg  [7:0] y
);
    always @(*) begin
        case (x)
        8'h00: y = 8'h52; 8'h01: y = 8'h09; 8'h02: y = 8'h6a; 8'h03: y = 8'hd5; 8'h04: y = 8'h30; 8'h05: y = 8'h36; 8'h06: y = 8'ha5; 8'h07: y = 8'h38;
        8'h08: y = 8'hbf; 8'h09: y = 8'h40; 8'h0a: y = 8'ha3; 8'h0b: y = 8'h9e; 8'h0c: y = 8'h81; 8'h0d: y = 8'hf3; 8'h0e: y = 8'hd7; 8'h0f: y = 8'hfb;
        8'h10: y = 8'h7c; 8'h11: y = 8'he3; 8'h12: y = 8'h39; 8'h13: y = 8'h82; 8'h14: y = 8'h9b; 8'h15: y = 8'h2f; 8'h16: y = 8'hff; 8'h17: y = 8'h87;
        8'h18: y = 8'h34; 8'h19: y = 8'h8e; 8'h1a: y = 8'h43; 8'h1b: y = 8'h44; 8'h1c: y = 8'hc4; 8'h1d: y = 8'hde; 8'h1e: y = 8'he9; 8'h1f: y = 8'hcb;
        8'h20: y = 8'h54; 8'h21: y = 8'h7b; 8'h22: y = 8'h94; 8'h23: y = 8'h32; 8'h24: y = 8'ha6; 8'h25: y = 8'hc2; 8'h26: y = 8'h23; 8'h27: y = 8'h3d;
        8'h28: y = 8'hee; 8'h29: y = 8'h4c; 8'h2a: y = 8'h95; 8'h2b: y = 8'h0b; 8'h2c: y = 8'h42; 8'h2d: y = 8'hfa; 8'h2e: y = 8'hc3; 8'h2f: y = 8'h4e;
        8'h30: y = 8'h08; 8'h31: y = 8'h2e; 8'h32: y = 8'ha1; 8'h33: y = 8'h66; 8'h34: y = 8'h28; 8'h35: y = 8'hd9; 8'h36: y = 8'h24; 8'h37: y = 8'hb2;
        8'h38: y = 8'h76; 8'h39: y = 8'h5b; 8'h3a: y = 8'ha2; 8'h3b: y = 8'h49; 8'h3c: y = 8'h6d; 8'h3d: y = 8'h8b; 8'h3e: y = 8'hd1; 8'h3f: y = 8'h25;
        8'h40: y = 8'h72; 8'h41: y = 8'hf8; 8'h42: y = 8'hf6; 8'h43: y = 8'h64; 8'h44: y = 8'h86; 8'h45: y = 8'h68; 8'h46: y = 8'h98; 8'h47: y = 8'h16;
        8'h48: y = 8'hd4; 8'h49: y = 8'ha4; 8'h4a: y = 8'h5c; 8'h4b: y = 8'hcc; 8'h4c: y = 8'h5d; 8'h4d: y = 8'h65; 8'h4e: y = 8'hb6; 8'h4f: y = 8'h92;
        8'h50: y = 8'h6c; 8'h51: y = 8'h70; 8'h52: y = 8'h48; 8'h53: y = 8'h50; 8'h54: y = 8'hfd; 8'h55: y = 8'hed; 8'h56: y = 8'hb9; 8'h57: y = 8'hda;
        8'h58: y = 8'h5e; 8'h59: y = 8'h15; 8'h5a: y = 8'h46; 8'h5b: y = 8'h57; 8'h5c: y = 8'ha7; 8'h5d: y = 8'h8d; 8'h5e: y = 8'h9d; 8'h5f: y = 8'h84;
        8'h60: y = 8'h90; 8'h61: y = 8'hd8; 8'h62: y = 8'hab; 8'h63: y = 8'h00; 8'h64: y = 8'h8c; 8'h65: y = 8'hbc; 8'h66: y = 8'hd3; 8'h67: y = 8'h0a;
        8'h68: y = 8'hf7; 8'h69: y = 8'he4; 8'h6a: y = 8'h58; 8'h6b: y = 8'h05; 8'h6c: y = 8'hb8; 8'h6d: y = 8'hb3; 8'h6e: y = 8'h45; 8'h6f: y = 8'h06;
        8'h70: y = 8'hd0; 8'h71: y = 8'h2c; 8'h72: y = 8'h1e; 8'h73: y = 8'h8f; 8'h74: y = 8'hca; 8'h75: y = 8'h3f; 8'h76: y = 8'h0f; 8'h77: y = 8'h02;
        8'h78: y = 8'hc1; 8'h79: y = 8'haf; 8'h7a: y = 8'hbd; 8'h7b: y = 8'h03; 8'h7c: y = 8'h01; 8'h7d: y = 8'h13; 8'h7e: y = 8'h8a; 8'h7f: y = 8'h6b;
        8'h80: y = 8'h3a; 8'h81: y = 8'h91; 8'h82: y = 8'h11; 8'h83: y = 8'h41; 8'h84: y = 8'h4f; 8'h85: y = 8'h67; 8'h86: y = 8'hdc; 8'h87: y = 8'hea;
        8'h88: y = 8'h97; 8'h89: y = 8'hf2; 8'h8a: y = 8'hcf; 8'h8b: y = 8'hce; 8'h8c: y = 8'hf0; 8'h8d: y = 8'hb4; 8'h8e: y = 8'he6; 8'h8f: y = 8'h73;
        8'h90: y = 8'h96; 8'h91: y = 8'hac; 8'h92: y = 8'h74; 8'h93: y = 8'h22; 8'h94: y = 8'he7; 8'h95: y = 8'had; 8'h96: y = 8'h35; 8'h97: y = 8'h85;
        8'h98: y = 8'he2; 8'h99: y = 8'hf9; 8'h9a: y = 8'h37; 8'h9b: y = 8'he8; 8'h9c: y = 8'h1c; 8'h9d: y = 8'h75; 8'h9e: y = 8'hdf; 8'h9f: y = 8'h6e;
        8'ha0: y = 8'h47; 8'ha1: y = 8'hf1; 8'ha2: y = 8'h1a; 8'ha3: y = 8'h71; 8'ha4: y = 8'h1d; 8'ha5: y = 8'h29; 8'ha6: y = 8'hc5; 8'ha7: y = 8'h89;
        8'ha8: y = 8'h6f; 8'ha9: y = 8'hb7; 8'haa: y = 8'h62; 8'hab: y = 8'h0e; 8'hac: y = 8'haa; 8'had: y = 8'h18; 8'hae: y = 8'hbe; 8'haf: y = 8'h1b;
        8'hb0: y = 8'hfc; 8'hb1: y = 8'h56; 8'hb2: y = 8'h3e; 8'hb3: y = 8'h4b; 8'hb4: y = 8'hc6; 8'hb5: y = 8'hd2; 8'hb6: y = 8'h79; 8'hb7: y = 8'h20;
        8'hb8: y = 8'h9a; 8'hb9: y = 8'hdb; 8'hba: y = 8'hc0; 8'hbb: y = 8'hfe; 8'hbc: y = 8'h78; 8'hbd: y = 8'hcd; 8'hbe: y = 8'h5a; 8'hbf: y = 8'hf4;
        8'hc0: y = 8'h1f; 8'hc1: y = 8'hdd; 8'hc2: y = 8'ha8; 8'hc3: y = 8'h33; 8'hc4: y = 8'h88; 8'hc5: y = 8'h07; 8'hc6: y = 8'hc7; 8'hc7: y = 8'h31;
        8'hc8: y = 8'hb1; 8'hc9: y = 8'h12; 8'hca: y = 8'h10; 8'hcb: y = 8'h59; 8'hcc: y = 8'h27; 8'hcd: y = 8'h80; 8'hce: y = 8'hec; 8'hcf: y = 8'h5f;
        8'hd0: y = 8'h60; 8'hd1: y = 8'h51; 8'hd2: y = 8'h7f; 8'hd3: y = 8'ha9; 8'hd4: y = 8'h19; 8'hd5: y = 8'hb5; 8'hd6: y = 8'h4a; 8'hd7: y = 8'h0d;
        8'hd8: y = 8'h2d; 8'hd9: y = 8'he5; 8'hda: y = 8'h7a; 8'hdb: y = 8'h9f; 8'hdc: y = 8'h93; 8'hdd: y = 8'hc9; 8'hde: y = 8'h9c; 8'hdf: y = 8'hef;
        8'he0: y = 8'ha0; 8'he1: y = 8'he0; 8'he2: y = 8'h3b; 8'he3: y = 8'h4d; 8'he4: y = 8'hae; 8'he5: y = 8'h2a; 8'he6: y = 8'hf5; 8'he7: y = 8'hb0;
        8'he8: y = 8'hc8; 8'he9: y = 8'heb; 8'hea: y = 8'hbb; 8'heb: y = 8'h3c; 8'hec: y = 8'h83; 8'hed: y = 8'h53; 8'hee: y = 8'h99; 8'hef: y = 8'h61;
        8'hf0: y = 8'h17; 8'hf1: y = 8'h2b; 8'hf2: y = 8'h04; 8'hf3: y = 8'h7e; 8'hf4: y = 8'hba; 8'hf5: y = 8'h77; 8'hf6: y = 8'hd6; 8'hf7: y = 8'h26;
        8'hf8: y = 8'he1; 8'hf9: y = 8'h69; 8'hfa: y = 8'h14; 8'hfb: y = 8'h63; 8'hfc: y = 8'h55; 8'hfd: y = 8'h21; 8'hfe: y = 8'h0c; 8'hff: y = 8'h7d;
        default: y = 8'h00;
        endcase
    end
endmodule

// SM4 的 S 盒（GB/T 32907-2016 §6.2）
module sm4_sbox (
    input  wire [7:0] x,
    output reg  [7:0] y
);
    always @(*) begin
        case (x)
        8'h00: y = 8'hd6; 8'h01: y = 8'h90; 8'h02: y = 8'he9; 8'h03: y = 8'hfe; 8'h04: y = 8'hcc; 8'h05: y = 8'he1; 8'h06: y = 8'h3d; 8'h07: y = 8'hb7;
        8'h08: y = 8'h16; 8'h09: y = 8'hb6; 8'h0a: y = 8'h14; 8'h0b: y = 8'hc2; 8'h0c: y = 8'h28; 8'h0d: y = 8'hfb; 8'h0e: y = 8'h2c; 8'h0f: y = 8'h05;
        8'h10: y = 8'h2b; 8'h11: y = 8'h67; 8'h12: y = 8'h9a; 8'h13: y = 8'h76; 8'h14: y = 8'h2a; 8'h15: y = 8'hbe; 8'h16: y = 8'h04; 8'h17: y = 8'hc3;
        8'h18: y = 8'haa; 8'h19: y = 8'h44; 8'h1a: y = 8'h13; 8'h1b: y = 8'h26; 8'h1c: y = 8'h49; 8'h1d: y = 8'h86; 8'h1e: y = 8'h06; 8'h1f: y = 8'h99;
        8'h20: y = 8'h9c; 8'h21: y = 8'h42; 8'h22: y = 8'h50; 8'h23: y = 8'hf4; 8'h24: y = 8'h91; 8'h25: y = 8'hef; 8'h26: y = 8'h98; 8'h27: y = 8'h7a;
        8'h28: y = 8'h33; 8'h29: y = 8'h54; 8'h2a: y = 8'h0b; 8'h2b: y = 8'h43; 8'h2c: y = 8'hed; 8'h2d: y = 8'hcf; 8'h2e: y = 8'hac; 8'h2f: y = 8'h62;
        8'h30: y = 8'he4; 8'h31: y = 8'hb3; 8'h32: y = 8'h1c; 8'h33: y = 8'ha9; 8'h34: y = 8'hc9; 8'h35: y = 8'h08; 8'h36: y = 8'he8; 8'h37: y = 8'h95;
        8'h38: y = 8'h80; 8'h39: y = 8'hdf; 8'h3a: y = 8'h94; 8'h3b: y = 8'hfa; 8'h3c: y = 8'h75; 8'h3d: y = 8'h8f; 8'h3e: y = 8'h3f; 8'h3f: y = 8'ha6;
        8'h40: y = 8'h47; 8'h41: y = 8'h07; 8'h42: y = 8'ha7; 8'h43: y = 8'hfc; 8'h44: y = 8'hf3; 8'h45: y = 8'h73; 8'h46: y = 8'h17; 8'h47: y = 8'hba;
        8'h48: y = 8'h83; 8'h49: y = 8'h59; 8'h4a: y = 8'h3c; 8'h4b: y = 8'h19; 8'h4c: y = 8'he6; 8'h4d: y = 8'h85; 8'h4e: y = 8'h4f; 8'h4f: y = 8'ha8;
        8'h50: y = 8'h68; 8'h51: y = 8'h6b; 8'h52: y = 8'h81; 8'h53: y = 8'hb2; 8'h54: y = 8'h71; 8'h55: y = 8'h64; 8'h56: y = 8'hda; 8'h57: y = 8'h8b;
        8'h58: y = 8'hf8; 8'h59: y = 8'heb; 8'h5a: y = 8'h0f; 8'h5b: y = 8'h4b; 8'h5c: y = 8'h70; 8'h5d: y = 8'h56; 8'h5e: y = 8'h9d; 8'h5f: y = 8'h35;
        8'h60: y = 8'h1e; 8'h61: y = 8'h24; 8'h62: y = 8'h0e; 8'h63: y = 8'h5e; 8'h64: y = 8'h63; 8'h65: y = 8'h58; 8'h66: y = 8'hd1; 8'h67: y = 8'ha2;
        8'h68: y = 8'h25; 8'h69: y = 8'h22; 8'h6a: y = 8'h7c; 8'h6b: y = 8'h3b; 8'h6c: y = 8'h01; 8'h6d: y = 8'h21; 8'h6e: y = 8'h78; 8'h6f: y = 8'h87;
        8'h70: y = 8'hd4; 8'h71: y = 8'h00; 8'h72: y = 8'h46; 8'h73: y = 8'h57; 8'h74: y = 8'h9f; 8'h75: y = 8'hd3; 8'h76: y = 8'h27; 8'h77: y = 8'h52;
        8'h78: y = 8'h4c; 8'h79: y = 8'h36; 8'h7a: y = 8'h02; 8'h7b: y = 8'he7; 8'h7c: y = 8'ha0; 8'h7d: y = 8'hc4; 8'h7e: y = 8'hc8; 8'h7f: y = 8'h9e;
        8'h80: y = 8'hea; 8'h81: y = 8'hbf; 8'h82: y = 8'h8a; 8'h83: y = 8'hd2; 8'h84: y = 8'h40; 8'h85: y = 8'hc7; 8'h86: y = 8'h38; 8'h87: y = 8'hb5;
        8'h88: y = 8'ha3; 8'h89: y = 8'hf7; 8'h8a: y = 8'hf2; 8'h8b: y = 8'hce; 8'h8c: y = 8'hf9; 8'h8d: y = 8'h61; 8'h8e: y = 8'h15; 8'h8f: y = 8'ha1;
        8'h90: y = 8'he0; 8'h91: y = 8'hae; 8'h92: y = 8'h5d; 8'h93: y = 8'ha4; 8'h94: y = 8'h9b; 8'h95: y = 8'h34; 8'h96: y = 8'h1a; 8'h97: y = 8'h55;
        8'h98: y = 8'had; 8'h99: y = 8'h93; 8'h9a: y = 8'h32; 8'h9b: y = 8'h30; 8'h9c: y = 8'hf5; 8'h9d: y = 8'h8c; 8'h9e: y = 8'hb1; 8'h9f: y = 8'he3;
        8'ha0: y = 8'h1d; 8'ha1: y = 8'hf6; 8'ha2: y = 8'he2; 8'ha3: y = 8'h2e; 8'ha4: y = 8'h82; 8'ha5: y = 8'h66; 8'ha6: y = 8'hca; 8'ha7: y = 8'h60;
        8'ha8: y = 8'hc0; 8'ha9: y = 8'h29; 8'haa: y = 8'h23; 8'hab: y = 8'hab; 8'hac: y = 8'h0d; 8'had: y = 8'h53; 8'hae: y = 8'h4e; 8'haf: y = 8'h6f;
        8'hb0: y = 8'hd5; 8'hb1: y = 8'hdb; 8'hb2: y = 8'h37; 8'hb3: y = 8'h45; 8'hb4: y = 8'hde; 8'hb5: y = 8'hfd; 8'hb6: y = 8'h8e; 8'hb7: y = 8'h2f;
        8'hb8: y = 8'h03; 8'hb9: y = 8'hff; 8'hba: y = 8'h6a; 8'hbb: y = 8'h72; 8'hbc: y = 8'h6d; 8'hbd: y = 8'h6c; 8'hbe: y = 8'h5b; 8'hbf: y = 8'h51;
        8'hc0: y = 8'h8d; 8'hc1: y = 8'h1b; 8'hc2: y = 8'haf; 8'hc3: y = 8'h92; 8'hc4: y = 8'hbb; 8'hc5: y = 8'hdd; 8'hc6: y = 8'hbc; 8'hc7: y = 8'h7f;
        8'hc8: y = 8'h11; 8'hc9: y = 8'hd9; 8'hca: y = 8'h5c; 8'hcb: y = 8'h41; 8'hcc: y = 8'h1f; 8'hcd: y = 8'h10; 8'hce: y = 8'h5a; 8'hcf: y = 8'hd8;
        8'hd0: y = 8'h0a; 8'hd1: y = 8'hc1; 8'hd2: y = 8'h31; 8'hd3: y = 8'h88; 8'hd4: y = 8'ha5; 8'hd5: y = 8'hcd; 8'hd6: y = 8'h7b; 8'hd7: y = 8'hbd;
        8'hd8: y = 8'h2d; 8'hd9: y = 8'h74; 8'hda: y = 8'hd0; 8'hdb: y = 8'h12; 8'hdc: y = 8'hb8; 8'hdd: y = 8'he5; 8'hde: y = 8'hb4; 8'hdf: y = 8'hb0;
        8'he0: y = 8'h89; 8'he1: y = 8'h69; 8'he2: y = 8'h97; 8'he3: y = 8'h4a; 8'he4: y = 8'h0c; 8'he5: y = 8'h96; 8'he6: y = 8'h77; 8'he7: y = 8'h7e;
        8'he8: y = 8'h65; 8'he9: y = 8'hb9; 8'hea: y = 8'hf1; 8'heb: y = 8'h09; 8'hec: y = 8'hc5; 8'hed: y = 8'h6e; 8'hee: y = 8'hc6; 8'hef: y = 8'h84;
        8'hf0: y = 8'h18; 8'hf1: y = 8'hf0; 8'hf2: y = 8'h7d; 8'hf3: y = 8'hec; 8'hf4: y = 8'h3a; 8'hf5: y = 8'hdc; 8'hf6: y = 8'h4d; 8'hf7: y = 8'h20;
        8'hf8: y = 8'h79; 8'hf9: y = 8'hee; 8'hfa: y = 8'h5f; 8'hfb: y = 8'h3e; 8'hfc: y = 8'hd7; 8'hfd: y = 8'hcb; 8'hfe: y = 8'h39; 8'hff: y = 8'h48;
        default: y = 8'h00;
        endcase
    end
endmodule

`default_nettype wire

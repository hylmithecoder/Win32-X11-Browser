// OpenCL kernel for GPU-accelerated base64 encode/decode.
// Each work-item processes one block: 4 base64 chars -> 3 bytes (decode)
// or 3 bytes -> 4 base64 chars (encode).

__constant uchar kDecodeTable[256] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,  // 0-15
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,  // 16-31
    64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,  // 32-47  (+ and /)
    52,53,54,55,56,57,58,59,60,61,64,64,64, 0,64,64,  // 48-63  0-9 and =
    64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,  // 64-79  A-O
    15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,  // 80-95  P-Z
    64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,  // 96-111 a-o
    41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,  // 112-127 p-z
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64
};

__constant char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Decode base64: each work-item decodes 4 input chars into 3 output bytes.
// src_len is the total number of base64 characters (excluding padding).
// dst_len is the total number of output bytes.
__kernel void base64_decode(__global const uchar *src, uint src_len,
                            __global uchar *dst, uint dst_len) {
    uint gid = get_global_id(0);
    uint in_offset = gid * 4;
    uint out_offset = gid * 3;

    if (in_offset + 4 > src_len) {
        return;
    }

    uchar c0 = kDecodeTable[src[in_offset + 0]];
    uchar c1 = kDecodeTable[src[in_offset + 1]];
    uchar c2 = kDecodeTable[src[in_offset + 2]];
    uchar c3 = kDecodeTable[src[in_offset + 3]];

    uint val = ((uint)c0 << 18) | ((uint)c1 << 12) | ((uint)c2 << 6) | (uint)c3;

    if (out_offset < dst_len) {
        dst[out_offset + 0] = (uchar)(val >> 16);
    }
    if (out_offset + 1 < dst_len) {
        dst[out_offset + 1] = (uchar)(val >> 8);
    }
    if (out_offset + 2 < dst_len) {
        dst[out_offset + 2] = (uchar)(val);
    }
}

// Encode to base64: each work-item encodes 3 input bytes into 4 output chars.
// src_len is the total number of input bytes.
// dst_len is the total number of output characters (including padding).
__kernel void base64_encode(__global const uchar *src, uint src_len,
                            __global uchar *dst, uint dst_len) {
    uint gid = get_global_id(0);
    uint in_offset = gid * 3;
    uint out_offset = gid * 4;

    if (in_offset >= src_len || out_offset + 4 > dst_len) {
        return;
    }

    uint val = (uint)src[in_offset] << 16;
    if (in_offset + 1 < src_len) {
        val |= (uint)src[in_offset + 1] << 8;
    }
    if (in_offset + 2 < src_len) {
        val |= (uint)src[in_offset + 2];
    }

    dst[out_offset + 0] = kEncodeTable[(val >> 18) & 0x3F];
    dst[out_offset + 1] = kEncodeTable[(val >> 12) & 0x3F];
    dst[out_offset + 2] = (in_offset + 1 < src_len)
        ? kEncodeTable[(val >> 6) & 0x3F] : '=';
    dst[out_offset + 3] = (in_offset + 2 < src_len)
        ? kEncodeTable[val & 0x3F] : '=';
}

/*
 *  Copyright (c) 2024, Peter Haag
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *   * Neither the name of SWITCH nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "digest/sha256.h"
#include "ja4.h"
#include "ssl/ssl.h"
#include "util.h"

#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))

ja4_t *ja4sProcess(ssl_t *ssl, uint8_t proto) {
    if (!ssl || ssl->type != SERVERssl) return NULL;

    ja4_t *ja4 = malloc(sizeof(ja4_t) + SIZEja4sString + 1);
    if (ja4 == NULL) {
        LogError("malloc() error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno));
        return NULL;
    }
    ja4->type = TYPE_UNDEF;
    ja4->string[0] = '\0';

    char *buff = ja4->string;
    // create ja4s_a
    buff[0] = proto == IPPROTO_TCP ? 't' : 'q';
    buff[1] = ssl->tlsCharVersion[0];
    buff[2] = ssl->tlsCharVersion[1];

    uint32_t num = LenArray(ssl->extensions);
    if (num > 99) return 0;
    uint32_t ones = num % 10;
    uint32_t tens = num / 10;
    buff[3] = tens + '0';
    buff[4] = ones + '0';

    if (ssl->alpnName[0]) {
        // first and last char
        buff[5] = ssl->alpnName[0];
        buff[6] = ssl->alpnName[strlen(ssl->alpnName) - 1];
    } else {
        buff[5] = '0';
        buff[6] = '0';
    }
    buff[7] = '_';

    // create ja4s_b
    char hexChars[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    uint16_t cipher = 0;
    uint32_t numCipher = LenArray(ssl->cipherSuites);
    if (numCipher == 1) {
        cipher = ArrayElement(ssl->cipherSuites, 0);
    }
    buff[8] = hexChars[cipher >> 12];
    buff[9] = hexChars[(cipher >> 8) & 0xF];
    buff[10] = hexChars[(cipher >> 4) & 0xF];
    buff[11] = hexChars[cipher & 0xF];
    buff[12] = '_';

    // create ja4s_c
    // generate string to sha256
    // create a string big enough for ciphersuites and extensions
    // uint16_t = max 5 digits + ',' = 6 digits per cipher + '\0'
    size_t maxStrLen = LenArray(ssl->extensions) * 6 + 1;
    char *hashString = (char *)malloc(maxStrLen);
    hashString[0] = '0';

    uint32_t index = 0;
    for (int i = 0; i < LenArray(ssl->extensions); i++) {
        uint16_t val = ArrayElement(ssl->extensions, i);
        hashString[index++] = hexChars[val >> 12];
        hashString[index++] = hexChars[(val >> 8) & 0xF];
        hashString[index++] = hexChars[(val >> 4) & 0xF];
        hashString[index++] = hexChars[val & 0xF];
        hashString[index++] = ',';
    }
    // overwrite last ',' with end of string
    hashString[index - 1] = '\0';

    uint8_t sha256Digest[32];
    char sha256String[65];
    sha256((const unsigned char *)hashString, strlen(hashString), (unsigned char *)sha256Digest);

#ifdef DEVEL
    HexString(sha256Digest, 32, sha256String);
    printf("CipherString: %s\n", hashString);
    printf("   Digest: %s\n", sha256String);
#else
    HexString(sha256Digest, 6, sha256String);
#endif

    memcpy((void *)(buff + 13), (void *)sha256String, 12);
    buff[25] = '\0';

    ja4->type = TYPE_JA4S;
    return ja4;

}  // End of ja4Process

// ex. ja4s: t130200_1301_234ea6891581
// input validation  - is ja4sString valid?
int ja4sCheck(char *ja4sString) {
    if (ja4sString == NULL || strlen(ja4sString) != SIZEja4sString) return 0;
    if (ja4sString[0] != 't' && ja4sString[0] != 'q') return 0;
    if (ja4sString[7] != '_' || ja4sString[12] != '_') return 0;

    for (int i = 1; i < 7; i++)
        if (!isascii(ja4sString[i])) return 0;
    for (int i = 8; i < 12; i++)
        if (!isxdigit(ja4sString[i])) return 0;
    for (int i = 13; i < SIZEja4sString; i++)
        if (!isxdigit(ja4sString[i])) return 0;
    return 1;
}  // End of ja4Check

#ifdef MAIN

int main(int argc, char **argv) {
    static const uint8_t srvHello[] = {
        0x16, 0x03,                                      // ..H.....
        0x03, 0x00, 0x7a, 0x02, 0x00, 0x00, 0x76, 0x03,  // ..z...v.
        0x03, 0x89, 0x7c, 0x23, 0x2e, 0x3e, 0xe3, 0x13,  // ..|#.>..
        0x31, 0x4f, 0x2b, 0x66, 0x23, 0x07, 0xff, 0x4f,  // 1O+f#..O
        0x7e, 0x2c, 0xf1, 0xca, 0xee, 0xc1, 0xb2, 0x77,  // ~,.....w
        0x11, 0xbc, 0xa7, 0x7f, 0x46, 0x95, 0x19, 0x16,  // ....F...
        0x85, 0x20, 0xbc, 0x58, 0xb9, 0x2f, 0x86, 0x5e,  // . .X./.^
        0x6b, 0x9a, 0xa4, 0xa6, 0x37, 0x1c, 0xad, 0xcb,  // k...7...
        0x0a, 0xfe, 0x1d, 0xa1, 0xc0, 0xf7, 0x05, 0x20,  // .......
        0x9a, 0x11, 0xd5, 0x23, 0x57, 0xf5, 0x6d, 0x5d,  // ...#W.m]
        0xd9, 0x62, 0x13, 0x01, 0x00, 0x00, 0x2e, 0x00,  // .b......
        0x33, 0x00, 0x24, 0x00, 0x1d, 0x00, 0x20, 0x76,  // 3.$... v
        0xb8, 0xb7, 0xed, 0x0f, 0x96, 0xb6, 0x3a, 0x77,  // ......:w
        0x3d, 0x85, 0xab, 0x6f, 0x3a, 0x87, 0xa1, 0x51,  // =..o:..Q
        0xc1, 0x30, 0x52, 0x97, 0x85, 0xb4, 0x1a, 0x4d,  // .0R....M
        0xef, 0xb5, 0x31, 0x84, 0x05, 0x59, 0x57, 0x00,  // ..1..YW.
        0x2b, 0x00, 0x02, 0x03, 0x04, 0x14, 0x03, 0x03,  // +.......
        0x00, 0x01, 0x01, 0x17, 0x03, 0x03, 0x12, 0x72,  // .......r
        0xde, 0xbd, 0x1a, 0x5c, 0x2b, 0x07, 0xc5, 0xf6,  // ...\+...
        0xc5, 0xdd, 0xca, 0xa2, 0x43, 0xa2, 0x8f, 0x00,  // ....C...
        0x80, 0xa8, 0xf7, 0x0d, 0xe8, 0x0a, 0xb5, 0x4a,  // .......J
        0x4e, 0x89, 0x03, 0xd7, 0x1b, 0xd8, 0x64, 0xcc,  // N.....d.
        0x11, 0xac, 0x5e, 0x80, 0xed, 0xf7, 0x2c, 0x5f,  // ..^...,_
        0xa1, 0xb7, 0xb4, 0x5f, 0xee, 0x57, 0x28, 0x7b,  // ..._.W({
        0x32, 0xf3, 0xca, 0x12, 0x5a, 0xb8, 0x87, 0x81,  // 2...Z...
        0xe0, 0xfe, 0x86, 0x1a, 0x58, 0x23, 0x35, 0x1b,  // ....X#5.
        0xac, 0xbf, 0xe6, 0x96, 0x19, 0x23, 0x71, 0xaa,  // .....#q.
        0xf2, 0x0b, 0x04, 0x9f, 0x29, 0x9c, 0x7c, 0x5b,  // ....).|[
        0xc9, 0x58, 0x62, 0xee, 0x8f, 0xa3, 0xcb, 0x6d,  // .Xb....m
        0x2a, 0x6c, 0x41, 0x3f, 0x61, 0xda, 0x99, 0x9b,  // *lA?a...
        0x90, 0x8a, 0x32, 0x44, 0x25, 0x98, 0x34, 0x03,  // ..2D%.4.
        0x29, 0x4a, 0xe0, 0xf6, 0x73, 0x4c, 0x61, 0x1d,  // )J..sLa.
        0xce, 0xd7, 0x98, 0x73, 0xd6, 0x66, 0x7d, 0xbd,  // ...s.f}.
        0xb7, 0x38, 0x3b, 0x65, 0xa9, 0xa9, 0x77, 0xbd,  // .8;e..w.
        0x85, 0x68, 0x11, 0x26, 0x81, 0x6f, 0x85, 0xf0,  // .h.&.o..
        0x93, 0x26, 0xdd, 0xfd, 0xad, 0xf6, 0xce, 0xfe,  // .&......
        0xf8, 0x63, 0x37, 0xfb, 0xf4, 0xab, 0xb4, 0xef,  // .c7.....
        0xd4, 0xc5, 0x2c, 0x24, 0x01, 0x69, 0x94, 0xd9,  // ..,$.i..
        0xe9, 0x2d, 0x9b, 0x0d, 0x8d, 0x45, 0x99, 0x55,  // .-...E.U
        0xad, 0xcf, 0x7e, 0xf8, 0xc6, 0xb5, 0xb6, 0x32,  // ..~....2
        0x49, 0x31, 0x13, 0x10, 0x11, 0x0f, 0xdf, 0xe8,  // I1......
        0x8d, 0x24, 0x31, 0x11, 0x19, 0xa5, 0x93, 0xe1,  // .$1.....
        0xf8, 0x7f, 0x08, 0x11, 0xbc, 0x6d, 0x14, 0x67,  // .....m.g
        0x77, 0x74, 0xdf, 0xf9, 0xcb, 0x77, 0x99, 0x04,  // wt...w..
        0xe4, 0xef, 0x18, 0x07, 0x64, 0x23, 0x54, 0x1f,  // ....d#T.
        0x75, 0x92, 0xa0, 0xce, 0x60, 0x8f, 0x60, 0x94,  // u...`.`.
        0x4e, 0x6a, 0xde, 0xac, 0xf9, 0x08, 0x22, 0x48,  // Nj...."H
        0xdb, 0x83, 0xb1, 0x9c, 0x3e, 0x80, 0x44, 0xc4,  // ....>.D.
        0x84, 0x68, 0xe6, 0x65, 0xec, 0x94, 0x3f, 0x09,  // .h.e..?.
        0x43, 0x59, 0xd2, 0x65, 0x5a, 0x98, 0x93, 0xad,  // CY.eZ...
        0x56, 0xdc, 0x31, 0x41, 0xb5, 0x7c, 0x11, 0xed,  // V.1A.|..
        0x4f, 0x32, 0xaa, 0x39, 0x04, 0xcd, 0xb5, 0xe7,  // O2.9....
        0xea, 0xd0, 0x4d, 0x82, 0xf8, 0x39, 0x58, 0x48,  // ..M..9XH
        0xc7, 0xb4, 0xdd, 0x16, 0xb5, 0x38, 0x0f, 0x42,  // .....8.B
        0x02, 0x70, 0xe5, 0xae, 0x0a, 0xe1, 0xbd, 0xf3,  // .p......
        0x5b, 0xcc, 0xae, 0x33, 0xce, 0x98, 0x85, 0xc8,  // [..3....
        0x9e, 0x15, 0xac, 0x06, 0xe4, 0xbe, 0x1d, 0xea,  // ........
        0x3a, 0xac, 0xa3, 0xc9, 0x9a, 0x41, 0x44, 0xf9,  // :....AD.
        0x27, 0xea, 0x05, 0x1c, 0x43, 0xeb, 0xdb, 0x9c,  // '...C...
        0x74, 0x8e, 0xe4, 0x99, 0x75, 0x02, 0xd4, 0xec,  // t...u...
        0x8c, 0x0e, 0x20, 0x81, 0xdc, 0x2d, 0x04, 0x0d,  // .. ..-..
        0xb0, 0xa2, 0x90, 0xec, 0x26, 0xc9, 0xc3, 0x38,  // ....&..8
        0x66, 0x82, 0xc8, 0xea, 0x35, 0x99, 0xae, 0x78,  // f...5..x
        0x42, 0x06, 0xbf, 0xc0, 0xf6, 0x46, 0x3a, 0x5b,  // B....F:[
        0x1d, 0x0f, 0x39, 0xf3, 0x1d, 0x17, 0x97, 0xed,  // ..9.....
        0x8b, 0xd4, 0x90, 0x34, 0xcf, 0xa2, 0xe6, 0x58,  // ...4...X
        0x4d, 0xbc, 0xb6, 0xc4, 0x0c, 0x4a, 0x67, 0x64,  // M....Jgd
        0x5c, 0x33, 0x80, 0x19, 0x37, 0x39, 0xbb, 0x6b,  // \3..79.k
        0x95, 0x32, 0xab, 0xfb, 0x9e, 0xcf, 0x7b, 0x1d,  // .2....{.
        0xe2, 0x3a, 0x46, 0xcb, 0x2c, 0xbe, 0x06, 0xff,  // .:F.,...
        0x6e, 0x71, 0x63, 0xb2, 0x4b, 0xb2, 0xf1, 0xcc,  // nqc.K...
        0xea, 0xb3, 0xa4, 0x9d, 0xd9, 0x09, 0x07, 0xa0,  // ........
        0xf0, 0x5a, 0x1f, 0xc6, 0x20, 0xcc, 0xb8, 0x34,  // .Z.. ..4
        0x7d, 0x5e, 0xf6, 0xf9, 0x0d, 0x3d, 0xc4, 0x0c,  // }^...=..
        0x12, 0x88, 0x30, 0x5a, 0xee, 0x31, 0x96, 0x4c,  // ..0Z.1.L
        0x2d, 0xde, 0xe6, 0x6c, 0xe9, 0x87, 0xd8, 0xc4,  // -..l....
        0x2f, 0xc1, 0xf6, 0xaa, 0x3c, 0x42, 0x6b, 0xf5,  // /...<Bk.
        0x4c, 0xac, 0x3f, 0xa4, 0x96, 0xa4, 0xaa, 0xaf,  // L.?.....
        0xad, 0xf7, 0x10, 0x1a, 0xda, 0x0c, 0x4e, 0x3c,  // ......N<
        0xdc, 0x77, 0x23, 0xc4, 0xce, 0x5f, 0x0b, 0xf7,  // .w#.._..
        0x05, 0xb3, 0xc3, 0xe6, 0xd7, 0x52, 0xbc, 0x80,  // .....R..
        0x16, 0x23, 0xca, 0x85, 0x55, 0x08, 0x71, 0x99,  // .#..U.q.
        0x4b, 0x12, 0x40, 0xbd, 0x62, 0x91, 0xc5, 0x76,  // K.@.b..v
        0x99, 0xf3, 0x62, 0x53, 0x1f, 0xb4, 0xfb, 0x98,  // ..bS....
        0x60, 0x60, 0x03, 0xad, 0x33, 0xdd, 0x52, 0x88,  // ``..3.R.
        0xa2, 0x67, 0x45, 0x37, 0x91, 0xe5, 0x71, 0x55,  // .gE7..qU
        0x3c, 0xf0, 0xee, 0xa8, 0x2c, 0x02, 0xf8, 0x15,  // <...,...
        0xe9, 0xc7, 0x52, 0xb8, 0x59, 0x18, 0x01, 0xc7,  // ..R.Y...
        0xd9, 0x4a, 0xe3, 0x13, 0x4e, 0x21, 0x34, 0xcb,  // .J..N!4.
        0xef, 0x67, 0x7f, 0xb7, 0xd9, 0x50, 0xcd, 0xa1,  // .g...P..
        0xf2, 0x90, 0x73, 0xb6, 0xc8, 0xef, 0x8f, 0x52,  // ..s....R
        0x04, 0x2e, 0x9c, 0x47, 0x2c, 0x60, 0x1c, 0xc2,  // ...G,`..
        0x3a, 0x6c, 0xff, 0x89, 0xb6, 0xc2, 0x7d, 0x13,  // :l....}.
        0x7e, 0x81, 0x0a, 0xf1, 0xf2, 0xd2, 0xa4, 0xb5,  // ~.......
        0x70, 0x9d, 0xa0, 0x68, 0x4e, 0x78, 0x33, 0xf1,  // p..hNx3.
        0x3b, 0x41, 0xee, 0xac, 0x18, 0xf7, 0x5d, 0xe6,  // ;A....].
        0xbc, 0xe4, 0x4f, 0x0c, 0x28, 0x80, 0xd4, 0xd1,  // ..O.(...
        0xfd, 0x7f, 0x8d, 0x49, 0x96, 0xd8, 0x10, 0x51,  // ...I...Q
        0x05, 0x4a, 0x81, 0x0e, 0x94, 0xac, 0x4a, 0x0d,  // .J....J.
        0x6d, 0x31, 0x24, 0xf0, 0x5b, 0x19, 0x1a, 0xc4,  // m1$.[...
        0xdb, 0x73, 0x34, 0xc8, 0x55, 0xb1, 0xcb, 0xc4,  // .s4.U...
        0xb2, 0xd1, 0x13, 0x8f, 0xaa, 0x5a, 0xd2, 0xe2,  // .....Z..
        0xbe, 0x79, 0x37, 0xdb, 0x7d, 0x9d, 0x7e, 0x62,  // .y7.}.~b
        0xba, 0x90, 0x10, 0x90, 0x23, 0x0b, 0x1f, 0xac,  // ....#...
        0x31, 0xd7, 0x55, 0x46, 0x3a, 0x6d, 0x65, 0x63,  // 1.UF:mec
        0x65, 0xc5, 0x67, 0x24, 0xe9, 0x3d, 0xf7, 0x36,  // e.g$.=.6
        0xf6, 0x13, 0x13, 0xb3, 0xb3, 0xc6, 0x24, 0x6c,  // ......$l
        0xfe, 0x83, 0x36, 0x2a, 0x66, 0x97, 0x21, 0xe7,  // ..6*f.!.
        0x24, 0x5e, 0xba, 0xfd, 0x48, 0x4d, 0xd7, 0x58,  // $^..HM.X
        0xa8, 0x89, 0xe3, 0xa1, 0xd5, 0x7c, 0x01, 0x8a,  // .....|..
        0xf2, 0xbc, 0x15, 0xb4, 0xe7, 0xa6, 0xe8, 0xee,  // ........
        0x12, 0xae, 0x96, 0x5b, 0x10, 0x9b, 0x8a, 0x6b,  // ...[...k
        0x67, 0x6b, 0x11, 0x67, 0xff, 0xf6, 0x1a, 0x6d,  // gk.g...m
        0x24, 0xc8, 0x15, 0x0a, 0x02, 0x6d, 0x8f, 0x62,  // $....m.b
        0x55, 0x00, 0x1d, 0xe1, 0x70, 0xfc, 0x06, 0xf9,  // U...p...
        0x9f, 0x08, 0xbd, 0x3d, 0x43, 0x54, 0x8a, 0x8b,  // ...=CT..
        0xfa, 0x81, 0x1f, 0x53, 0xa1, 0x39, 0xbe, 0xd4,  // ...S.9..
        0xc0, 0x2e, 0xc4, 0x21, 0x48, 0x68, 0xe8, 0x37,  // ...!Hh.7
        0xa6, 0x85, 0xec, 0x30, 0xda, 0x46, 0xdf, 0xf3,  // ...0.F..
        0xd6, 0x10, 0x13, 0x45, 0xf6, 0x81, 0x1b, 0xec,  // ...E....
        0xca, 0xe9, 0xdf, 0xaf, 0xcd, 0x72, 0x7f, 0xad,  // .....r..
        0x18, 0x1b, 0xd4, 0x77, 0x18, 0xbe, 0x84, 0x23,  // ...w...#
        0x25, 0x06, 0x1c, 0x7b, 0x36, 0x80, 0xc9, 0xf0,  // %..{6...
        0x52, 0x71, 0x25, 0xfa, 0x17, 0xb5, 0x78, 0x12,  // Rq%...x.
        0x18, 0x60, 0xb1, 0x35, 0x59, 0x7e, 0x17, 0x66,  // .`.5Y~.f
        0x3f, 0x50, 0x99, 0xda, 0xbd, 0x50, 0x42, 0x9f,  // ?P...PB.
        0x9b, 0xa0, 0x74, 0xb2, 0x7b, 0x45, 0xde, 0x81,  // ..t.{E..
        0xae, 0xec, 0x2a, 0x4b, 0xce, 0x1f, 0x64, 0x29,  // ..*K..d)
        0xf6, 0xfa, 0x37, 0x8e, 0xdb, 0x7c, 0x2b, 0x83,  // ..7..|+.
        0x38, 0xbe, 0x26, 0x13, 0x45, 0x40, 0xc2, 0xf5,  // 8.&.E@..
        0x35, 0x1e, 0x40, 0x1c, 0x2a, 0xf0, 0x51, 0x56,  // 5.@.*.QV
        0x02, 0x9f, 0x1d, 0x63, 0xaa, 0x1e, 0x2f, 0x85,  // ...c../.
        0xc3, 0x12, 0xea, 0xee, 0xbf, 0x5e, 0x74, 0x4e,  // .....^tN
        0x87, 0xe0, 0x95, 0xf6, 0x8d, 0x59, 0x10, 0x97,  // .....Y..
        0x30, 0xb6, 0xae, 0x7e, 0x4f, 0xf5, 0xa6, 0x6c,  // 0..~O..l
        0xf9, 0xed, 0x29, 0xcf, 0x8c, 0x04, 0x5f, 0x48,  // ..)..._H
        0x63, 0xd0, 0x72, 0xa7, 0x35, 0x44, 0xd7, 0xef,  // c.r.5D..
        0xc0, 0xf2, 0x00, 0x08, 0x11, 0x10, 0x31, 0x79,  // ......1y
        0x89, 0x35, 0xd2, 0x54, 0xb5, 0x67, 0x59, 0x1e,  // .5.T.gY.
        0xc1, 0x10, 0x53, 0xf4, 0x40, 0xb9, 0xf0, 0xa6,  // ..S.@...
        0x80, 0xbc, 0x7f, 0x6b, 0x54, 0x1c, 0x6a, 0x29,  // ...kT.j)
        0xb7, 0xd5, 0xe8, 0x3e, 0x42, 0xe8, 0x7a, 0xdb,  // ...>B.z.
        0x1c, 0x13, 0xd6, 0x2b, 0x71, 0xce, 0xbb, 0x91,  // ...+q...
        0x75, 0x26, 0x7e, 0x7a, 0xc0, 0x83, 0x1f, 0xfe,  // u&~z....
        0xa7, 0x57, 0x3e, 0x65, 0x5b, 0xcd, 0x27, 0x75,  // .W>e[.'u
        0x99, 0xf1, 0x28, 0x2e, 0x48, 0x9b, 0x98, 0xdf,  // ..(.H...
        0xb1, 0xe2, 0xca, 0x0a, 0x3f, 0x9e, 0x2c, 0x59,  // ....?.,Y
        0xd3, 0xfb, 0x40, 0x3f, 0xf8, 0x80, 0x73, 0x90,  // ..@?..s.
        0x68, 0x92, 0xb7, 0x47, 0xc7, 0xf6, 0x04, 0xcb,  // h..G....
        0xef, 0x24, 0x1a, 0x62, 0x86, 0x25, 0x0e, 0xa3,  // .$.b.%..
        0x59, 0x59, 0x9c, 0xf8, 0xa5, 0x1b, 0x07, 0x66,  // YY.....f
        0x2c, 0x59, 0x37, 0x15, 0xd1, 0x04, 0x14, 0xbe,  // ,Y7.....
        0x65, 0x17, 0x06, 0xa8, 0x9f, 0x77, 0x1e, 0x13,  // e....w..
        0xf5, 0xb5, 0x63, 0x29, 0x88, 0x0a, 0x8e, 0x0d,  // ..c)....
        0x24, 0xb4, 0x1b, 0xde, 0xcc, 0x17, 0x44, 0x40,  // $.....D@
        0x46, 0x2b, 0xa5, 0x23, 0x79, 0xf8, 0x1b, 0x2a,  // F+.#y..*
        0x0a, 0xce, 0xe1, 0xf5, 0x06, 0xdb, 0xe8, 0x91,  // ........
        0x86, 0x08, 0x3e, 0xa4, 0x1d, 0xc2, 0xfd, 0xc9,  // ..>.....
        0xd8, 0x7f, 0xe0, 0x82, 0xcf, 0x24, 0xd5, 0xb2,  // .....$..
        0xd9, 0x27, 0xa2, 0xe5, 0x88, 0xc2, 0xa0, 0x47,  // .'.....G
        0xe4, 0xb9, 0xc6, 0x7b, 0xcd, 0x34, 0xfd, 0x8a,  // ...{.4..
        0x3c, 0xed, 0xdd, 0x11, 0x9e, 0xa3, 0xe4, 0x23,  // <......#
        0xfc, 0xb6, 0x77, 0x1f, 0x99, 0xab, 0x31, 0x67,  // ..w...1g
        0x6e, 0x47, 0x7f, 0xd3, 0xea, 0xf7, 0x81, 0xda,  // nG......
        0x71, 0x12, 0x40, 0x7f, 0x84, 0x6e, 0x23, 0xc0,  // q.@..n#.
        0x1d, 0x78, 0xa7, 0xdc, 0x7b, 0x13, 0x58, 0x0c,  // .x..{.X.
        0x02, 0x1c, 0xfc, 0x69, 0xe5, 0x96, 0x93, 0xdf,  // ...i....
        0x47, 0x5b, 0x96, 0x85, 0xe1, 0x58, 0xa6, 0xe1,  // G[...X..
        0x4d, 0xdb, 0x85, 0x9d, 0x5d, 0xa1, 0xe5, 0x4c,  // M...]..L
        0x8b, 0x9b, 0x70, 0xce, 0x32, 0x27, 0x8c, 0xee,  // ..p.2'..
        0x99, 0xed, 0x86, 0xda, 0x63, 0xad, 0x31, 0x6a,  // ....c.1j
        0xc5, 0xc3, 0xef, 0xf6, 0x75, 0x35, 0x30, 0x2e,  // ....u50.
        0x67, 0x2d, 0xef, 0xd6, 0xa0, 0xee, 0x36, 0x24,  // g-....6$
        0xfc, 0x16, 0xd2, 0xf6, 0xaf, 0xdd, 0xd2, 0xa3,  // ........
        0xa8, 0xbb, 0x58, 0x40, 0x1e, 0x91, 0x7f, 0xc0,  // ..X@....
        0x49, 0x4d, 0x8b, 0x00, 0xcf, 0xe8, 0xfb, 0x90,  // IM......
        0x90, 0x5b, 0x60, 0xba, 0x0c, 0xc2, 0x97, 0xd3,  // .[`.....
        0xb5, 0x7d                                       // .}
    };

    // ja4s: t130200_1301_234ea6891581

    size_t len = sizeof(srvHello);

    ssl_t *ssl = sslProcess(srvHello, len);
    if (!ssl) {
        printf("Failed to parse ssl\n");
        exit(255);
    }
    ja4_t *ja4 = ja4sProcess(ssl, IPPROTO_TCP);
    if (ja4)
        printf("ja4s: %s\n", ja4->string);
    else
        printf("Failed to parse ja4s\n");

    return 0;
}

#endif

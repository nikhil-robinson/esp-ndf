| layer | esp-ndf cycles | ESP-NN 1.4.1 cycles | gain | bit-exact |
|---|---|---|---|---|
| conv1x1 32x32x16->32 | 1007821 | 1138795 | 1.13x | yes |
| conv3x3 16x16x16->32 p1 | 577203 | 628073 | 1.09x | yes |
| dwconv3x3 32x32x32 s1 | 1244040 | 1255262 | 1.01x | yes |
| dwconv3x3 32x32x32 s2 | 313925 | 333788 | 1.06x | yes |
| fc 512->64 | 11948 | 26693 | 2.23x | yes |
| maxpool 2x2 32x32x16 | 16292 | 46268 | 2.84x | yes |
| avgpool 2x2 32x32x16 | 23716 | 120049 | 5.06x | yes |
| add 4096 | 125892 | 153649 | 1.22x | yes |
| relu6 4096 | 1953 | 3904 | 2.00x | yes |
| softmax 100x10 | 120516 | 226620 | 1.88x | yes |

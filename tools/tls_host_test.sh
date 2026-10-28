#!/bin/bash
# TLS engine against OpenSSL on the host: our client against `openssl s_server`, our server against `openssl s_client`; wrong CA, wrong name and
# a pinned leaf; 1 MB downloaded and uploaded. usage: tools/tls_host_test.sh   (needs openssl and python3 cryptography)
set -e
cd "$(dirname "$0")/.."
D=build/tls; mkdir -p $D; python3 tools/wl/tls_certs.py $D >/dev/null
F="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Itools/fuzz/host -Icomponents/include -Icomponents/soc/esp32s3 -Icomponents/tls/include"
gcc $F tools/tls_host.c components/tls/tls.c components/tls/tls_crypto.c components/tls/x509.c components/lib/ecc.c components/lib/p256_comb.c -lcrypto -o $D/tls_host
CA=$(cat $D/ca_pub.hex); OTHER=$(cat $D/other_ca_pub.hex); PUB=$(cat $D/server_pub.hex); PRIV=$(cat $D/server_priv.hex)
$D/tls_host selftest
head -c 1048576 /dev/urandom > $D/www_big.bin; mkdir -p $D/www; cp $D/www_big.bin $D/www/big.bin; echo -n "hello tls" > $D/www/hello.txt
SUM=$(python3 -c "print(sum(open('$D/www_big.bin','rb').read()))")
fail=0; check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got '$2' want '$3'"; fail=1; fi; }
PORT=$((20000 + RANDOM % 20000))
R=$PWD; (cd $D/www && exec openssl s_server -accept $PORT -cert $R/$D/server.pem -key $R/$D/server_key.pem -tls1_3 -ciphersuites TLS_AES_128_GCM_SHA256 -groups X25519 -sigalgs ECDSA+SHA256 -WWW -quiet >/dev/null 2>&1) &
SP=$!; sleep 0.5
r=$($D/tls_host client 127.0.0.1 $PORT $CA ndf.local /hello.txt | tail -1); check "client: GET /hello.txt (CA pinned, name checked)" "$r" "status HTTP/1.0 200 body 9 sum $(python3 -c "print(sum(b'hello tls'))")"
r=$($D/tls_host client 127.0.0.1 $PORT $CA a.example.test /hello.txt | tail -1); check "client: wildcard SAN accepts a.example.test" "${r%% body*}" "status HTTP/1.0 200"
r=$($D/tls_host client 127.0.0.1 $PORT $CA ndf.local /big.bin | tail -1); check "client: 1 MB download intact" "$r" "status HTTP/1.0 200 body 1048576 sum $SUM"
r=$($D/tls_host client 127.0.0.1 $PORT - ndf.local /hello.txt $PUB | tail -1); check "client: pinned leaf key (no CA)" "${r%% body*}" "status HTTP/1.0 200"
r=$($D/tls_host client 127.0.0.1 $PORT $OTHER ndf.local /hello.txt | tail -1); check "client: wrong CA refused" "${r%% -*}" "handshake failed"
r=$($D/tls_host client 127.0.0.1 $PORT $CA evil.example.com /hello.txt | tail -1); check "client: wrong name refused" "${r%% -*}" "handshake failed"
kill $SP 2>/dev/null || true; wait $SP 2>/dev/null || true
# our server against openssl s_client (it closes at the end of its input; the server's own log carries the byte count)
$D/tls_host server $PORT $D/server.der $PRIV 1048576 > $D/srv.out & SP=$!; sleep 0.5
head -c 1048576 $D/www_big.bin | openssl s_client -connect 127.0.0.1:$PORT -tls1_3 -ciphersuites TLS_AES_128_GCM_SHA256 -groups X25519 -CAfile $D/ca.pem -verify_hostname ndf.local -verify_return_error -max_send_frag 4096 -quiet >/dev/null 2>&1 || true
wait $SP 2>/dev/null || true
check "server: 1 MB upload from openssl s_client received intact" "$(tail -1 $D/srv.out)" "got 1048576 bytes sum $SUM"
$D/tls_host server $PORT $D/server.der $PRIV > $D/srv.out & SP=$!; sleep 0.5
head -c 200000 $D/www_big.bin | openssl s_client -connect 127.0.0.1:$PORT -tls1_3 -ciphersuites TLS_AES_128_GCM_SHA256 -groups X25519 -CAfile $D/ca.pem -quiet >/dev/null 2>&1 || true
wait $SP 2>/dev/null || true
check "server: a 16 KB record (above our limit) fails closed with an alert" "$(tail -1 $D/srv.out | cut -c1-30)" "read error -8"
$D/tls_host server $PORT $D/server.der $PRIV > $D/srv.out & SP=$!; sleep 0.5
r=$(echo hi | openssl s_client -connect 127.0.0.1:$PORT -tls1_3 -CAfile $D/other_ca.pem -verify_return_error -quiet 2>&1 | grep -c "verify error" || true)
kill $SP 2>/dev/null || true; wait $SP 2>/dev/null || true
check "server: openssl client with the wrong CA gives up" "$([ "$r" -ge 1 ] && echo yes || echo no)" "yes"
[ $fail = 0 ] && echo "=== tls host test ALL PASS ===" || { echo "=== tls host test FAILED ==="; exit 1; }

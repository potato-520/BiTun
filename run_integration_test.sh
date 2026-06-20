#!/bin/bash
set -e

# Compile project before running tests
make -C src/linux

echo "=================================================="
echo "Starting BiTun Integration Test (Pure Bidirectional SOCKS5)..."
echo "=================================================="

# 1. Start a local HTTP server
echo "[Test] Starting Python HTTP target server on port 8000..."
python3 -m http.server 8000 > /tmp/target_server.log 2>&1 &
HTTP_PID=$!
sleep 1

# Ensure cleanup on exit
cleanup() {
    echo "[Test] Cleaning up processes..."
    kill $HTTP_PID 2>/dev/null || true
    kill $PEER_A_PID 2>/dev/null || true
    kill $PEER_B_PID 2>/dev/null || true
    rm -f /tmp/target_server.log /tmp/peer_a.log /tmp/peer_b.log /tmp/curl_a_output.txt /tmp/curl_b_output.txt
}
trap cleanup EXIT

# 2. Start Peer A (SOCKS5 Proxy on port 9000, generates ODD channel IDs)
echo "[Test] Starting Peer A (SOCKS5 Proxy on port 9000)..."
src/linux/bitun -p 9000 -k MySecretPSKKey123456789012345678 --odd > /tmp/peer_a.log 2>&1 &
PEER_A_PID=$!
sleep 1

# 3. Start Peer B (SOCKS5 Proxy on port 9001, connects to Peer A on 9000, generates EVEN channel IDs)
echo "[Test] Starting Peer B (SOCKS5 Proxy on port 9001)..."
src/linux/bitun -p 9001 -r 127.0.0.1:9000 -k MySecretPSKKey123456789012345678 --even > /tmp/peer_b.log 2>&1 &
PEER_B_PID=$!

echo "[Test] Waiting for handshake to establish..."
sleep 4

# 4. Perform SOCKS5 query via Peer A
echo "[Test] Testing tunnel: Client -> Peer A (SOCKS5 9000) -> Peer B (Egress) -> HTTP Server (8000)..."
if curl -s --socks5-hostname 127.0.0.1:9000 http://127.0.0.1:8000/ > /tmp/curl_a_output.txt; then
    echo "[Test] Peer A Curl completed successfully!"
    echo "[Test] Received response:"
    head -n 5 /tmp/curl_a_output.txt
    echo "..."
else
    echo "[Test] ERROR: Peer A Curl failed!"
    echo "--- Peer A Log ---"
    cat /tmp/peer_a.log
    echo "--- Peer B Log ---"
    cat /tmp/peer_b.log
    exit 1
fi

# 5. Perform SOCKS5 query via Peer B (testing bidirectionality)
echo "[Test] Testing tunnel: Client -> Peer B (SOCKS5 9001) -> Peer A (Egress) -> HTTP Server (8000)..."
if curl -s --socks5-hostname 127.0.0.1:9001 http://127.0.0.1:8000/ > /tmp/curl_b_output.txt; then
    echo "[Test] Peer B Curl completed successfully!"
    echo "[Test] Received response:"
    head -n 5 /tmp/curl_b_output.txt
    echo "..."
    echo "[Test] SUCCESS: BiTun Integration Test Passed!"
else
    echo "[Test] ERROR: Peer B Curl failed!"
    echo "--- Peer A Log ---"
    cat /tmp/peer_a.log
    echo "--- Peer B Log ---"
    cat /tmp/peer_b.log
    exit 1
fi


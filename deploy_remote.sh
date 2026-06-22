#!/usr/bin/env bash
set +e
KEY=/c/Users/Shadow/.ssh/id_ed25519_shadow_to_linux
echo "=== linux-a ==="
ssh -i $KEY agent@203.0.113.11 'sudo systemctl stop bsmesh; sleep 1; cd ~/bridgesessions && tar -xzf /tmp/bridgesessions-src.tgz && g++ -std=c++23 -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT -o bsmesh bridgesessions.cpp -lssl -lcrypto -lzstd -pthread -lfmt -lspdlog 2>/dev/null && chmod +x bsmesh && truncate -s0 ~/.bridgesessions/bs-mesh.log && sudo systemctl start bsmesh && sleep 1 && echo linux-a=$(systemctl is-active bsmesh)'
echo "=== linux-b ==="
ssh -i $KEY agent@203.0.113.12 'sudo systemctl stop bsmesh; sleep 1; cd ~/bridgesessions && tar -xzf /tmp/bridgesessions-src.tgz && g++ -std=c++23 -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT -o bsmesh bridgesessions.cpp -lssl -lcrypto -lzstd -pthread -lfmt -lspdlog 2>/dev/null && chmod +x bsmesh && truncate -s0 ~/.bridgesessions/bs-mesh.log && sudo systemctl start bsmesh && sleep 1 && echo linux-b=$(systemctl is-active bsmesh)'
echo "=== macos-peer ==="
ssh macos-peer 'launchctl unload ~/Library/LaunchAgents/com.bridgesessions.mesh.plist 2>/dev/null; pkill -f "bridgesessions --config" 2>/dev/null; sleep 2; cd ~/bridgesessions && tar -xzf /tmp/bridgesessions-src.tgz && /opt/homebrew/bin/cmake --build build -j >/dev/null 2>&1 && cp -f build/bridgesessions bridgesessions && truncate -s0 ~/.bridgesessions/bs-mesh.log && launchctl load ~/Library/LaunchAgents/com.bridgesessions.mesh.plist && sleep 2 && echo mac_listen=$(lsof -nP -iTCP:19949 -sTCP:LISTEN 2>/dev/null | grep -c LISTEN)'

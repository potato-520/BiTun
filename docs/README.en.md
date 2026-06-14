# BiTun (Bi-directional Tunnel)

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](../LICENSE)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![Language: C](https://img.shields.io/badge/Language-C-blue.svg)]()

**BiTun** is a lightweight, fully symmetric, point-to-point encrypted tunnel tool written in pure C. Operating over KCP and UDP, it provides high-performance duplex tunneling with robust NAT hole punching, connection migration, ChaCha20-Poly1305 AEAD encryption, and anti-replay protections.

Over a single encrypted UDP tunnel, BiTun supports **Dynamic SOCKS5 Proxies (equivalent to `ssh -D`)**, **Local static Port Forwarding (equivalent to `ssh -L`)**, and **Remote reverse Port Forwarding (equivalent to `ssh -R`)** concurrently.

> 🌐 **Multi-language Documentation**:
> *   [Chinese Version (中文版)](../README.md)
> *   [Japanese Version (日本語版)](README.ja.md)
> *   [System Design Specification (design.md)](design.md) (Design details located at `/home/chenming/BiTun/docs/design.md`)
> *   [Verification & Testing Report (verification_report.md)](verification_report.md)

---

## 📐 System Architecture & Data Flow

```mermaid
graph TD
    %% Define color styles
    classDef peerA fill:#1d3557,stroke:#457b9d,stroke-width:2px,color:#fff;
    classDef peerB fill:#2a9d8f,stroke:#264653,stroke-width:2px,color:#fff;
    classDef network fill:#e76f51,stroke:#f4a261,stroke-width:2px,color:#fff;
    
    subgraph PeerA ["Peer A (Symmetric Endpoint)"]
        A_TCP["Local TCP Listen / Connect<br>(SOCKS5 / Forward L / R)"]:::peerA
        A_Mux["Channel Multiplexing Layer<br>(Odd/Even IDs, Flow Control)"]:::peerA
        A_KCP["KCP Layer<br>(Low Latency Retransmit, Congestion Control)"]:::peerA
        A_AEAD["AEAD Security Shim<br>(ChaCha20-Poly1305, Anti-Replay)"]:::peerA
    end

    subgraph PeerB ["Peer B (Symmetric Endpoint)"]
        B_TCP["Local TCP Listen / Connect<br>(SOCKS5 / Forward L / R)"]:::peerB
        B_Mux["Channel Multiplexing Layer<br>(Odd/Even IDs, Flow Control)"]:::peerB
        B_KCP["KCP Layer<br>(Low Latency Retransmit, Congestion Control)"]:::peerB
        B_AEAD["AEAD Security Shim<br>(ChaCha20-Poly1305, Anti-Replay)"]:::peerB
    end

    %% Internal layer flows
    A_TCP <=> A_Mux
    A_Mux <=> A_KCP
    A_KCP <=> A_AEAD

    B_TCP <=> B_Mux
    B_Mux <=> B_KCP
    B_KCP <=> B_AEAD

    %% Network Transport
    A_AEAD <== "UDP Network Transport<br>(Symmetric Punching / Connection Migration)" ===> B_AEAD:::network
```

### Traffic Flow Patterns (Traffic Flows)

```mermaid
graph TD
    %% Style definitions
    classDef app fill:#e63946,stroke:#b11e31,stroke-width:1px,color:#fff;
    classDef bitunA fill:#1d3557,stroke:#457b9d,stroke-width:1px,color:#fff;
    classDef bitunB fill:#2a9d8f,stroke:#264653,stroke-width:1px,color:#fff;
    classDef dest fill:#2b2d42,stroke:#8d99ae,stroke-width:1px,color:#fff;

    %% Scenario 1
    subgraph Mode1 ["1. Dynamic SOCKS5 Proxy Mode (ssh -D)"]
        M1_Client["Browser / Client"]:::app -- "TCP (Negotiation)" --> M1_PeerA["Peer A (SOCKS5 Port)"]:::bitunA
        M1_PeerA -- "Encrypted KCP Tunnel" --> M1_PeerB["Peer B"]:::bitunB
        M1_PeerB -- "TCP Connection" --> M1_Target["Target Server"]:::dest
    end

    %% Scenario 2
    subgraph Mode2 ["2. Local Port Forwarding Mode (ssh -L)"]
        M2_Client["Local App"]:::app -- "TCP (Static Port)" --> M2_PeerA["Peer A (Local Listener)"]:::bitunA
        M2_PeerA -- "Encrypted KCP Tunnel" --> M2_PeerB["Peer B"]:::bitunB
        M2_PeerB -- "TCP Connection" --> M2_Target["Target Server"]:::dest
    end

    %% Scenario 3
    subgraph Mode3 ["3. Remote Port Forwarding Mode (ssh -R)"]
        M3_Client["Public User"]:::app -- "TCP (Public Port)" --> M3_PeerB["Peer B (Public Listener)"]:::bitunB
        M3_PeerB -- "Encrypted KCP Tunnel" --> M3_PeerA["Peer A"]:::bitunA
        M3_PeerA -- "TCP Connection" --> M3_Target["Local Target Service"]:::dest
    end
```

---

## 🚀 System Features

1. **Fully Symmetric Peer-to-Peer Architecture**
   * Both sides run identical code and state machines. There is no hardcoded Client/Server distinction.
   * Supports **Symmetric Active-Active Punching** and **Passive/Dynamic Learning** modes. In passive mode, a peer binds to the dynamic NAT mapping of the first valid incoming packet, enabling seamless point-to-point connections.
2. **KCP Reliability & Channel Multiplexing**
   * Integrates KCP over raw UDP, offering fast ARQ retransmission and low-latency transport.
   * Multiplexes multiple concurrent application connections over a single KCP tunnel. Employs Odd/Even Channel ID generation to prevent ID collisions.
3. **Cryptographic Defenses (AEAD & Anti-Replay)**
   * **Full Traffic AEAD**: Integrates ChaCha20-Poly1305 between UDP and KCP to encrypt and authenticate all packet payloads and control frames.
   * **Session Key Derivation**: Uses PSK only for initial challenge-response. Derives ephemeral session keys using **HKDF-SHA256** based on mutual random salts, completely mitigating Nonce-reuse issues upon device restarts.
   * **Sliding Window Anti-Replay**: Employs an IPsec-style 64-bit sliding window bitmap on the receiver side to silently discard replayed packets, preventing CPU/memory exhaustion DoS attacks.
4. **Authenticated Fast Reconnection (AUTH_RESET)**
   * When a peer reboots, it transmits a plaintext `AUTH_RESET` frame signed using the PSK. The active peer validates the timestamp (±5s drift tolerance) and HMAC signature.
   * If verified, it tears down the stale connection in milliseconds, resolving the 30-second hang-up delay typical in AEAD filters.
5. **Seamless Connection Migration**
   * If a peer's public IP/Port changes due to a network switch (Wi-Fi/Cellular) or symmetric NAT port mapping changes, VPS dynamically updates the target address upon successful AEAD decryption of any incoming packet.
   * KCP session states, sliding windows, and TCP client channels remain active and uninterrupted.
6. **Granular Flow Control & Backpressure**
   * **Sender Backpressure**: Monitors KCP's wait queue (`waitsnd >= 32`) to suspend local TCP reading. Combined with a 2KB read quota per channel tick, this prevents heap exhaustion and OOM.
   * **Channel-level Sliding Windows**: Employs SSH/HTTP2-style flow control (4KB windows pushed by `CMD_WINDOW_UPDATE` frames) to isolate congestion, completely avoiding Head-of-Line (HOL) blocking.

---

## 📂 Directory Layout

```text
.
├── LICENSE             # Open source license (Apache 2.0)
├── Makefile            # Build script
├── README.md           # Chinese README
├── docs/               # Documentation directory
│   ├── README.en.md    # English README (This document)
│   ├── README.ja.md    # Japanese README
│   ├── design.md       # System Design Specification
│   └── verification_report.md # Verification & Testing Report
└── src/                # Source code directory
    ├── encrypt.c/h     # AEAD encryption, HKDF, anti-replay sliding window
    ├── ikcp.c/h        # Core KCP protocol
    ├── socks5.c/h      # Stateless streaming SOCKS5 parser
    ├── tunnel.c/h      # Symmetric tunnel state machine, events, multiplexing, backpressure
    └── main.c          # CLI entry and config parser
```

---

## 🛠️ Compilation

### Prerequisites
* Linux Operating System
* GCC compiler & GNU Make
* OpenSSL development libraries (providing `libcrypto` for ChaCha20-Poly1305 and HMAC/HKDF)

### Build Command
Compile by running make in the root directory:
```bash
make
```
This generates the binary `bitun` in the root folder.

### Clean Command
```bash
make clean
```

---

## 📖 Usage Instructions

### Command Line Syntax
```text
bitun -m <mode> -p <local_port> [-r <remote_ip:remote_port>] [-t <target_ip:target_port>] -k <psk> [--odd | --even]
```
* `-m, --mode`：Operation mode. Options are `socks5`, `forward_l` (local port forwarding), and `forward_r` (remote port forwarding).
* `-p, --port`：Local UDP port. Also acts as the TCP listener port in `socks5` or `forward_l` modes.
* `-r, --remote`：Remote peer UDP endpoint (`IP:Port`). **Omit this parameter to run in passive dynamic learning mode**.
* `-t, --target`：Target mapping destination (`IP:Port`). Required in static forwarding modes.
* `-k, --psk`：Pre-shared key (automatically padded or truncated to 32 bytes).
* `--odd` / `--even`：Configures the peer to generate Odd or Even Channel IDs to prevent collisions. One side must be odd and the other must be even.

---

### 💡 Application Scenarios

#### Scenario 1: Symmetric SOCKS5 Proxy (Double Process Simulation)
* **Peer A** (SOCKS5 proxy listener on local port 9000, initiating connection to B, Odd IDs):
  ```bash
  ./bitun -m socks5 -p 9000 -r 127.0.0.1:9001 -k MySecretPSKKey123456789012345678 --odd
  ```
* **Peer B** (SOCKS5 proxy listener on local port 9001, initiating connection to A, Even IDs):
  ```bash
  ./bitun -m socks5 -p 9001 -r 127.0.0.1:9000 -k MySecretPSKKey123456789012345678 --even
  ```
* *Test*:
  Configure your browser or curl to use `127.0.0.1:9000` or `127.0.0.1:9001` as SOCKS5 proxies to surf the web.

#### Scenario 2: Active-Passive Hole Punching (VPS Hub & LAN Node)
* **VPS Side** (Listens on UDP 9000, dynamically learning the client NAT address):
  ```bash
  ./bitun -m socks5 -p 9000 -k MySecretPSKKey123456789012345678 --odd
  ```
* **LAN Side** (Listens on UDP 9001, actively punching to the public VPS):
  ```bash
  ./bitun -m socks5 -p 9001 -r <VPS_IP>:9000 -k MySecretPSKKey123456789012345678 --even
  ```

#### Scenario 3: Local Port Forwarding (equivalent to `ssh -L`)
To map local RDP connections (`127.0.0.1:3389`) to a remote target (`192.168.1.100:3389`):
* **Local Peer** (Listens on TCP 3389, forwarding connections to the tunnel):
  ```bash
  ./bitun -m forward_l -p 3389 -r <Remote_IP>:9001 -t 192.168.1.100:3389 -k MySecretPSKKey123456789012345678 --even
  ```
* **Remote Peer** (Listens on UDP 9001, accepting tunnel requests and connecting to the target):
  ```bash
  ./bitun -m socks5 -p 9001 -r <Local_IP>:3389 -k MySecretPSKKey123456789012345678 --odd
  ```

#### Scenario 4: Remote Port Forwarding (equivalent to `ssh -R`)
To listen on VPS port 8080 and forward incoming traffic back to a local web server (`127.0.0.1:80`):
* **VPS Peer** (Listens on TCP 8080, forwarding incoming TCP connections to the tunnel):
  ```bash
  ./bitun -m forward_r -p 8080 -k MySecretPSKKey123456789012345678 --odd -t 127.0.0.1:80
  ```
* **Local Peer** (Listens on UDP 9001, receiving tunnel packets and establishing local TCP to port 80):
  ```bash
  ./bitun -m socks5 -p 9001 -r <VPS_IP>:8080 -k MySecretPSKKey123456789012345678 --even
  ```

---

## 📄 License

This project is licensed under the [Apache License 2.0](../LICENSE).

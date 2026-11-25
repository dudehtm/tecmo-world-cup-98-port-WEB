// TWC98BO Puerta 4/5B WebRTC P2P Client: 1v1 Unreliable DataChannel + 3 Signaling Modes + STUN/TURN.
// Modes: 'local' (BroadcastChannel 2-tab), 'worker' (WebSocket Trickle ICE Relay + HTTP fallback), 'manual' (Deflate+Base64 SDP).

(() => {
  const DEFAULT_ICE = [
    { urls: 'stun:stun.l.google.com:19302' },
  ];

  // Cache ephemeral TURN credentials (4h server TTL) for 30m in-memory to skip redundant /ice roundtrips.
  const ICE_CACHE_TTL_MS = 30 * 60 * 1000;
  const iceCacheByUrl = new Map();
  const iceInFlightByUrl = new Map();

  async function compressToken(obj) {
    const json = JSON.stringify(obj);
    const bytes = new TextEncoder().encode(json);
    if (typeof CompressionStream === 'function') {
      const cs = new CompressionStream('deflate-raw');
      const stream = new Blob([bytes]).stream().pipeThrough(cs);
      const out = new Uint8Array(await new Response(stream).arrayBuffer());
      return 'Z1.' + u8ToB64Url(out);
    }
    return 'R1.' + u8ToB64Url(bytes);
  }

  async function decompressToken(str) {
    const clean = String(str || '').trim();
    if (!clean) throw new Error('Token SDP vacío');
    if (clean.startsWith('Z1.')) {
      const u8 = b64UrlToU8(clean.slice(3));
      const ds = new DecompressionStream('deflate-raw');
      const stream = new Blob([u8]).stream().pipeThrough(ds);
      const ab = await new Response(stream).arrayBuffer();
      return JSON.parse(new TextDecoder().decode(ab));
    }
    if (clean.startsWith('R1.')) {
      const u8 = b64UrlToU8(clean.slice(3));
      return JSON.parse(new TextDecoder().decode(u8));
    }
    return JSON.parse(clean);
  }

  function u8ToB64Url(u8) {
    let bin = '';
    for (let i = 0; i < u8.length; i++) bin += String.fromCharCode(u8[i]);
    return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }

  function b64UrlToU8(b64u) {
    let b64 = b64u.replace(/-/g, '+').replace(/_/g, '/');
    while (b64.length % 4) b64 += '=';
    const bin = atob(b64);
    const u8 = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
    return u8;
  }

  function recommendDelayForRtt(rttMs) {
    if (rttMs <= 25) return 1;
    if (rttMs <= 65) return 2;
    if (rttMs <= 115) return 3;
    if (rttMs <= 185) return 4;
    if (rttMs <= 240) return 5;
    return 6;
  }

  function sanitizeIceServers(servers) {
    const out = [];
    for (const srv of servers || []) {
      if (!srv || !srv.urls) continue;
      const list = (Array.isArray(srv.urls) ? srv.urls : [srv.urls])
        .filter((u) => typeof u === 'string' && !/:53(?:\?|$)/.test(u));
      if (list.length > 0) {
        out.push({ ...srv, urls: list.length === 1 && !Array.isArray(srv.urls) ? list[0] : list });
      }
    }
    return out;
  }

  async function fetchWorkerIceCached(workerUrl) {
    const base = String(workerUrl || '').trim().replace(/\/+$/, '');
    if (!base) return [];
    const now = Date.now();
    const cached = iceCacheByUrl.get(base);
    if (cached && (now - cached.ts) < ICE_CACHE_TTL_MS) {
      return cached.servers;
    }
    if (iceInFlightByUrl.has(base)) {
      return iceInFlightByUrl.get(base);
    }
    const reqPromise = (async () => {
      const ctrl = typeof AbortController === 'function' ? new AbortController() : null;
      const tm = ctrl ? setTimeout(() => ctrl.abort(), 3500) : null;
      try {
        const res = await fetch(`${base}/ice`, ctrl ? { signal: ctrl.signal } : undefined);
        if (res.ok) {
          const data = await res.json();
          const servers = Array.isArray(data.iceServers) ? sanitizeIceServers(data.iceServers) : [];
          if (servers.length > 0) {
            iceCacheByUrl.set(base, { servers, ts: Date.now() });
          }
          return servers;
        }
      } catch {
        // Optional endpoint; ignore if absent or timed out.
      } finally {
        if (tm) clearTimeout(tm);
        iceInFlightByUrl.delete(base);
      }
      return cached ? cached.servers : [];
    })();
    iceInFlightByUrl.set(base, reqPromise);
    return reqPromise;
  }

  function prefetchWorkerIce(workerUrl) {
    fetchWorkerIceCached(workerUrl).catch(() => {});
  }

  function toWsBaseUrl(httpUrl) {
    const clean = String(httpUrl || '').trim().replace(/\/+$/, '');
    if (/^https:\/\//i.test(clean)) return 'wss://' + clean.slice(8);
    if (/^http:\/\//i.test(clean)) return 'ws://' + clean.slice(7);
    return clean;
  }

  function waitIceComplete(pc, expectRelay = false, timeoutMs = 4000) {
    if (pc.iceGatheringState === 'complete') return Promise.resolve();
    return new Promise((resolve) => {
      let done = false;
      let hasSrflx = false;
      let hasRelay = false;
      let earlyTimer = null;
      const finish = () => {
        if (done) return;
        done = true;
        pc.removeEventListener('icegatheringstatechange', onState);
        pc.removeEventListener('icecandidate', onCand);
        clearTimeout(timer);
        if (earlyTimer) clearTimeout(earlyTimer);
        resolve();
      };
      const maybeEarlyFinish = () => {
        if (earlyTimer || !hasSrflx) return;
        if (!expectRelay || hasRelay) {
          // Allow 350ms for sibling interfaces/ports after srflx+relay appear, then proceed.
          earlyTimer = setTimeout(finish, 350);
        }
      };
      const onCand = (ev) => {
        const c = ev && ev.candidate && ev.candidate.candidate ? ev.candidate.candidate : '';
        if (c.includes(' typ srflx')) hasSrflx = true;
        if (c.includes(' typ relay')) hasRelay = true;
        maybeEarlyFinish();
      };
      const onState = () => {
        if (pc.iceGatheringState === 'complete') finish();
      };
      const timer = setTimeout(finish, timeoutMs);
      pc.addEventListener('icegatheringstatechange', onState);
      pc.addEventListener('icecandidate', onCand);
    });
  }

  class RtcP2PClient {
    constructor(callbacks = {}) {
      this.cb = callbacks;
      this.pc = null;
      this.dcInput = null;
      this.dcCtrl = null;
      this.bc = null;
      this.sigWs = null;
      this.sigPingTimer = null;
      this.role = null; // 'host' | 'guest'
      this.room = '';
      this.mode = 'local'; // 'local' | 'worker' | 'manual'
      this.workerUrl = '';
      this.manualIceServers = [];
      this.workerIceServers = [];
      this.iceConfig = { iceServers: [...DEFAULT_ICE] };
      this.closed = false;
      this.pollAborted = false;
      this.disconnectTimer = null;
      this.rttSamples = [];
      this.lastRttMs = 0;
      this.jitterMs = 0;
      this.txBytesWindow = 0;
      this.rxBytesWindow = 0;
      this.txKbps = 0;
      this.rxKbps = 0;
      this.kbpsTimer = null;
      this.pingTimer = null;
    }

    rebuildIceConfig() {
      this.iceConfig = {
        iceServers: sanitizeIceServers([
          ...DEFAULT_ICE,
          ...this.workerIceServers,
          ...this.manualIceServers,
        ]),
      };
    }

    hasTurnServer() {
      return this.iceConfig.iceServers.some((srv) => {
        const list = Array.isArray(srv.urls) ? srv.urls : [srv.urls];
        return list.some((u) => typeof u === 'string' && /^turns?:/i.test(u));
      });
    }

    setTurnConfig({ turnUrl, turnUser, turnCred }) {
      const manual = [];
      const u = String(turnUrl || '').trim();
      if (u) {
        const entry = { urls: u };
        if (turnUser) entry.username = String(turnUser).trim();
        if (turnCred) entry.credential = String(turnCred).trim();
        manual.push(entry);
      }
      this.manualIceServers = manual;
      this.rebuildIceConfig();
    }

    emitStatus(text, state = 'connecting') {
      this.cb.onStatus?.({ text, state });
    }

    async fetchWorkerIceIfAvailable() {
      const servers = await fetchWorkerIceCached(this.workerUrl);
      if (servers.length > 0) {
        this.workerIceServers = servers;
        this.rebuildIceConfig();
      }
    }

    closeSignalingWs() {
      if (this.sigPingTimer) {
        clearInterval(this.sigPingTimer);
        this.sigPingTimer = null;
      }
      if (this.sigWs) {
        const ws = this.sigWs;
        this.sigWs = null;
        try {
          ws.onopen = null;
          ws.onmessage = null;
          ws.onerror = null;
          ws.onclose = null;
          if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
            ws.close(1000, 'done');
          }
        } catch {}
      }
    }

    // Opens a buffered signaling WebSocket so early messages pushed immediately on 101 are never missed.
    openSignalingWs(role, timeoutMs = 4000) {
      if (typeof WebSocket !== 'function' || !this.workerUrl) {
        return Promise.resolve(null);
      }
      const wsBase = toWsBaseUrl(this.workerUrl);
      const wsUrl = `${wsBase}/room/${encodeURIComponent(this.room)}/ws?role=${encodeURIComponent(role)}`;

      return new Promise((resolve) => {
        let settled = false;
        let ws = null;
        const earlyQueue = [];
        let handler = null;
        let closeHandler = null;

        const finish = (result) => {
          if (settled) return;
          settled = true;
          clearTimeout(timer);
          resolve(result);
        };

        const timer = setTimeout(() => {
          try { ws?.close(); } catch {}
          finish(null);
        }, timeoutMs);

        try {
          ws = new WebSocket(wsUrl);
        } catch {
          finish(null);
          return;
        }

        ws.onmessage = (ev) => {
          let msg = null;
          try { msg = JSON.parse(ev.data); } catch { return; }
          if (!msg || typeof msg !== 'object') return;
          if (msg.t === 'pong') return;
          if (handler) {
            handler(msg);
          } else {
            earlyQueue.push(msg);
          }
        };

        ws.onopen = () => {
          if (this.closed || this.pollAborted) {
            try { ws.close(1000, 'aborted'); } catch {}
            finish(null);
            return;
          }
          const conn = {
            ws,
            send: (obj) => {
              if (ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify(obj));
                return true;
              }
              return false;
            },
            onMessage: (fn) => {
              handler = fn;
              while (earlyQueue.length > 0) {
                fn(earlyQueue.shift());
              }
            },
            onClose: (fn) => {
              closeHandler = fn;
              if (ws.readyState === WebSocket.CLOSED || ws.readyState === WebSocket.CLOSING) {
                fn();
              }
            },
          };
          finish(conn);
        };

        ws.onerror = () => {
          if (!settled) {
            try { ws.close(); } catch {}
            finish(null);
          }
        };

        ws.onclose = () => {
          if (!settled) {
            finish(null);
          } else if (closeHandler) {
            closeHandler();
          }
        };
      });
    }

    createPeerConnection() {
      this.closeTransportOnly();
      this.closed = false;
      this.pollAborted = false;
      this.rttSamples = [];
      this.lastRttMs = 0;
      this.jitterMs = 0;

      const pc = new RTCPeerConnection(this.iceConfig);
      this.pc = pc;

      const clearDiscTimer = () => {
        if (this.disconnectTimer) {
          clearTimeout(this.disconnectTimer);
          this.disconnectTimer = null;
        }
      };

      pc.oniceconnectionstatechange = () => {
        if (this.closed || this.pc !== pc) return;
        const st = pc.iceConnectionState;
        if (st === 'checking') {
          this.emitStatus('Negociando ruta P2P (ICE/TURN)...', 'connecting');
        } else if (st === 'connected' || st === 'completed') {
          clearDiscTimer();
          const bothOpen = Boolean(
            this.dcInput && this.dcCtrl &&
            this.dcInput.readyState === 'open' && this.dcCtrl.readyState === 'open'
          );
          this.emitStatus(bothOpen ? 'WebRTC P2P conectado' : 'Ruta P2P lista. Abriendo canales...', bothOpen ? 'connected' : 'connecting');
        } else if (st === 'disconnected') {
          // Transient ICE disconnection during path probing or Wi-Fi/mobile handoff: wait 5s before failing.
          this.emitStatus('WebRTC reconectando...', 'connecting');
          clearDiscTimer();
          this.disconnectTimer = setTimeout(() => {
            if (this.closed || this.pc !== pc) return;
            const cur = pc.iceConnectionState;
            if (cur === 'disconnected' || cur === 'failed' || cur === 'closed') {
              this.emitStatus(`WebRTC ${cur}`, cur);
              this.cb.onDisconnect?.(cur);
            }
          }, 5000);
        } else if (st === 'failed' || st === 'closed') {
          clearDiscTimer();
          this.emitStatus(`WebRTC ${st}`, st);
          this.cb.onDisconnect?.(st);
        }
      };

      if (this.role === 'host') {
        const dcIn = pc.createDataChannel('twc98-input', { ordered: false, maxRetransmits: 0 });
        const dcCtl = pc.createDataChannel('twc98-ctrl', { ordered: true });
        this.attachChannels(dcIn, dcCtl);
      } else {
        pc.ondatachannel = (ev) => {
          const ch = ev.channel;
          if (ch.label === 'twc98-input') this.attachInputChannel(ch);
          else if (ch.label === 'twc98-ctrl') this.attachCtrlChannel(ch);
        };
      }

      if (this.kbpsTimer) clearInterval(this.kbpsTimer);
      this.kbpsTimer = setInterval(() => {
        this.txKbps = Number(((this.txBytesWindow * 8) / 1000).toFixed(1));
        this.rxKbps = Number(((this.rxBytesWindow * 8) / 1000).toFixed(1));
        this.txBytesWindow = 0;
        this.rxBytesWindow = 0;
      }, 1000);

      return pc;
    }

    attachChannels(dcIn, dcCtl) {
      this.attachInputChannel(dcIn);
      this.attachCtrlChannel(dcCtl);
    }

    attachInputChannel(ch) {
      ch.binaryType = 'arraybuffer';
      this.dcInput = ch;
      ch.onopen = () => this.checkBothOpen();
      ch.onmessage = (ev) => {
        if (ev.data instanceof ArrayBuffer) {
          this.rxBytesWindow += ev.data.byteLength;
          this.cb.onInputPacket?.(ev.data);
        }
      };
    }

    attachCtrlChannel(ch) {
      this.dcCtrl = ch;
      ch.onopen = () => {
        this.checkBothOpen();
        if (this.pingTimer) clearInterval(this.pingTimer);
        this.pingTimer = setInterval(() => this.sendPing(), 1500);
        this.sendPing();
      };
      ch.onmessage = (ev) => {
        try {
          const msg = JSON.parse(ev.data);
          if (msg.t === 'ping') {
            this.sendCtrl({ t: 'pong', ts: msg.ts });
          } else if (msg.t === 'pong') {
            const rtt = Math.max(0, performance.now() - msg.ts);
            this.recordRtt(rtt);
          } else {
            this.cb.onControl?.(msg);
          }
        } catch {
          // Ignore malformed ctrl frame
        }
      };
    }

    checkBothOpen() {
      if (this.dcInput && this.dcCtrl && this.dcInput.readyState === 'open' && this.dcCtrl.readyState === 'open') {
        // Signaling is no longer needed once P2P DataChannels are established.
        this.closeSignalingWs();
        this.emitStatus('Canales P2P listos', 'open');
        this.cb.onOpen?.();
      }
    }

    recordRtt(rtt) {
      this.lastRttMs = Math.round(rtt);
      this.rttSamples.push(rtt);
      if (this.rttSamples.length > 10) this.rttSamples.shift();
      let min = Infinity;
      let max = 0;
      let sum = 0;
      for (const s of this.rttSamples) {
        if (s < min) min = s;
        if (s > max) max = s;
        sum += s;
      }
      const avg = sum / this.rttSamples.length;
      this.jitterMs = Math.round((max - min) / 2);
      this.cb.onRtt?.({
        rttMs: this.lastRttMs,
        avgRttMs: Math.round(avg),
        jitterMs: this.jitterMs,
        recommendedDelay: recommendDelayForRtt(Math.round(avg)),
      });
    }

    sendPing() {
      if (this.dcCtrl && this.dcCtrl.readyState === 'open') {
        this.sendCtrl({ t: 'ping', ts: performance.now() });
      }
    }

    sendCtrl(obj) {
      if (this.dcCtrl && this.dcCtrl.readyState === 'open') {
        this.dcCtrl.send(JSON.stringify(obj));
        return true;
      }
      return false;
    }

    sendInputBytes(buf) {
      if (this.dcInput && this.dcInput.readyState === 'open') {
        this.txBytesWindow += buf.byteLength;
        this.dcInput.send(buf);
        return true;
      }
      return false;
    }

    async hostStart({ mode, room, workerUrl, onManualOffer }) {
      this.role = 'host';
      this.mode = mode;
      this.room = String(room || '').trim().toUpperCase();
      this.workerUrl = String(workerUrl || '').trim().replace(/\/+$/, '');

      if (this.mode === 'worker') {
        if (!this.workerUrl) throw new Error('URL del Worker de señalización requerida');
        this.emitStatus(`Abriendo sala ${this.room}...`, 'connecting');

        // Fetch TURN credentials (cached) and open signaling WebSocket in parallel.
        const [, wsConn] = await Promise.all([
          this.fetchWorkerIceIfAvailable(),
          this.openSignalingWs('host', 4000),
        ]);
        if (this.closed || this.pollAborted) {
          try { wsConn?.ws?.close(1000, 'aborted'); } catch {}
          return null;
        }

        if (wsConn) {
          const pc = this.createPeerConnection();
          this.sigWs = wsConn.ws;
          this.sigPingTimer = setInterval(() => {
            if (this.sigWs === wsConn.ws && wsConn.ws.readyState === WebSocket.OPEN) {
              wsConn.send({ t: 'ping' });
            }
          }, 25000);

          const pendingRemoteIce = [];
          let remoteDescReady = false;
          const flushRemoteIce = async () => {
            while (pendingRemoteIce.length > 0 && !this.closed && this.pc === pc) {
              const cand = pendingRemoteIce.shift();
              try { await pc.addIceCandidate(cand); } catch {}
            }
          };

          pc.onicecandidate = (ev) => {
            if (ev && ev.candidate && this.sigWs === wsConn.ws && wsConn.ws.readyState === WebSocket.OPEN) {
              const cand = typeof ev.candidate.toJSON === 'function' ? ev.candidate.toJSON() : ev.candidate;
              wsConn.send({ t: 'ice', candidate: cand });
            }
          };

          wsConn.onMessage(async (msg) => {
            if (this.closed || this.pc !== pc) return;
            if (msg.t === 'answer' && msg.sdp && !pc.currentRemoteDescription) {
              try {
                this.emitStatus('Guest unido. Negociando ruta P2P...', 'connecting');
                await pc.setRemoteDescription(msg.sdp);
                remoteDescReady = true;
                await flushRemoteIce();
              } catch (err) {
                this.emitStatus(`Error aplicando Answer: ${err.message}`, 'failed');
              }
            } else if (msg.t === 'ice' && msg.candidate) {
              if (remoteDescReady) {
                try { await pc.addIceCandidate(msg.candidate); } catch {}
              } else {
                pendingRemoteIce.push(msg.candidate);
              }
            }
          });

          const offer = await pc.createOffer();
          await pc.setLocalDescription(offer);
          const localSdp = { type: pc.localDescription.type, sdp: pc.localDescription.sdp };
          if (!wsConn.send({ t: 'offer', sdp: localSdp })) {
            throw new Error('No se pudo publicar la sala en el Worker');
          }
          this.emitStatus(`Sala ${this.room} lista. Esperando Guest...`, 'waiting');
          return null;
        }

        // HTTP fallback (also backed by Durable Object with 0ms stale cache)
        const pc = this.createPeerConnection();
        this.emitStatus('Generando oferta ICE...', 'connecting');
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);
        await waitIceComplete(pc, this.hasTurnServer(), 4000);
        const localSdp = { type: pc.localDescription.type, sdp: pc.localDescription.sdp };

        this.emitStatus(`Publicando sala ${this.room} en Worker...`, 'waiting');
        const postRes = await fetch(`${this.workerUrl}/room/${encodeURIComponent(this.room)}/offer`, {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify({ sdp: localSdp }),
        });
        if (!postRes.ok) throw new Error(`Worker HTTP ${postRes.status}`);

        this.emitStatus(`Sala ${this.room} lista. Esperando Guest...`, 'waiting');
        while (!this.closed && !this.pollAborted && !pc.currentRemoteDescription) {
          await new Promise((r) => setTimeout(r, 300));
          if (this.closed || this.pollAborted) break;
          try {
            const gRes = await fetch(`${this.workerUrl}/room/${encodeURIComponent(this.room)}/answer`);
            if (gRes.status === 200) {
              const data = await gRes.json();
              if (data && data.sdp) {
                await pc.setRemoteDescription(data.sdp);
                this.emitStatus('Guest unido. Negociando ruta P2P...', 'connecting');
                break;
              }
            }
          } catch {
            // Retry polling
          }
        }
        return null;
      }

      const pc = this.createPeerConnection();
      this.emitStatus('Generando oferta ICE...', 'connecting');
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      await waitIceComplete(pc, this.hasTurnServer(), 4000);
      const localSdp = { type: pc.localDescription.type, sdp: pc.localDescription.sdp };

      if (this.mode === 'manual') {
        const token = await compressToken(localSdp);
        onManualOffer?.(token);
        this.emitStatus('Oferta lista. Envía el código al Guest y pega su Answer.', 'waiting');
        return token;
      }

      if (this.mode === 'local') {
        this.bc = new BroadcastChannel('twc98-rtc-room-v1');
        this.emitStatus(`Sala local ${this.room} abierta. Esperando Guest en otra pestaña...`, 'waiting');
        this.bc.onmessage = async (ev) => {
          const m = ev.data || {};
          if (m.room !== this.room) return;
          if (m.t === 'guest_hello') {
            this.bc.postMessage({ t: 'host_offer', room: this.room, sdp: localSdp });
          } else if (m.t === 'guest_answer' && m.sdp && !pc.currentRemoteDescription) {
            await pc.setRemoteDescription(m.sdp);
            this.emitStatus('Answer recibida. Conectando DataChannel...', 'connecting');
          }
        };
        // Proactively announce in case Guest joined 100ms earlier
        this.bc.postMessage({ t: 'host_offer', room: this.room, sdp: localSdp });
        return null;
      }
    }

    async hostAcceptManualAnswer(answerToken) {
      if (!this.pc) throw new Error('No hay sesión Host activa');
      const sdp = await decompressToken(answerToken);
      await this.pc.setRemoteDescription(sdp);
      this.emitStatus('Answer aplicada. Conectando P2P...', 'connecting');
    }

    async guestJoin({ mode, room, workerUrl, manualOfferToken, onManualAnswer }) {
      this.role = 'guest';
      this.mode = mode;
      this.room = String(room || '').trim().toUpperCase();
      this.workerUrl = String(workerUrl || '').trim().replace(/\/+$/, '');

      if (this.mode === 'worker') {
        if (!this.workerUrl) throw new Error('URL del Worker de señalización requerida');
        this.emitStatus(`Conectando a sala ${this.room}...`, 'waiting');

        // Fetch TURN credentials (cached) and open signaling WebSocket in parallel.
        const [, wsConn] = await Promise.all([
          this.fetchWorkerIceIfAvailable(),
          this.openSignalingWs('guest', 4000),
        ]);
        if (this.closed || this.pollAborted) {
          try { wsConn?.ws?.close(1000, 'aborted'); } catch {}
          return null;
        }

        if (wsConn) {
          const pc = this.createPeerConnection();
          this.sigWs = wsConn.ws;
          this.sigPingTimer = setInterval(() => {
            if (this.sigWs === wsConn.ws && wsConn.ws.readyState === WebSocket.OPEN) {
              wsConn.send({ t: 'ping' });
            }
          }, 25000);

          const pendingRemoteIce = [];
          let remoteDescReady = false;
          let answered = false;
          const flushRemoteIce = async () => {
            while (pendingRemoteIce.length > 0 && !this.closed && this.pc === pc) {
              const cand = pendingRemoteIce.shift();
              try { await pc.addIceCandidate(cand); } catch {}
            }
          };

          pc.onicecandidate = (ev) => {
            if (ev && ev.candidate && this.sigWs === wsConn.ws && wsConn.ws.readyState === WebSocket.OPEN) {
              const cand = typeof ev.candidate.toJSON === 'function' ? ev.candidate.toJSON() : ev.candidate;
              wsConn.send({ t: 'ice', candidate: cand });
            }
          };

          await new Promise((resolve, reject) => {
            let done = false;
            const finishOk = () => {
              if (done) return;
              done = true;
              clearTimeout(offerTimer);
              resolve();
            };
            const finishErr = (err) => {
              if (done) return;
              done = true;
              clearTimeout(offerTimer);
              reject(err);
            };

            const offerTimer = setTimeout(() => {
              finishErr(new Error(`Sala ${this.room} no encontrada o expirada`));
            }, 30000);

            wsConn.onClose(() => {
              if (!answered && !this.closed && !this.pollAborted) {
                finishErr(new Error(`Conexión cerrada buscando sala ${this.room}`));
              }
            });

            wsConn.onMessage(async (msg) => {
              if (this.closed || this.pc !== pc) return;
              if (msg.t === 'waiting_offer' && !answered) {
                this.emitStatus(`Esperando al Host en sala ${this.room}...`, 'waiting');
              } else if (msg.t === 'offer' && msg.sdp && !answered) {
                answered = true;
                try {
                  this.emitStatus('Sala encontrada. Generando respuesta...', 'connecting');
                  await pc.setRemoteDescription(msg.sdp);
                  remoteDescReady = true;
                  await flushRemoteIce();
                  const answer = await pc.createAnswer();
                  await pc.setLocalDescription(answer);
                  const localAnswer = { type: pc.localDescription.type, sdp: pc.localDescription.sdp };
                  if (!wsConn.send({ t: 'answer', sdp: localAnswer })) {
                    throw new Error('Error enviando Answer por WebSocket');
                  }
                  this.emitStatus('Respuesta enviada. Negociando ruta P2P...', 'connecting');
                  finishOk();
                } catch (err) {
                  finishErr(err);
                }
              } else if (msg.t === 'ice' && msg.candidate) {
                if (remoteDescReady) {
                  try { await pc.addIceCandidate(msg.candidate); } catch {}
                } else {
                  pendingRemoteIce.push(msg.candidate);
                }
              } else if (msg.t === 'error' && !answered) {
                finishErr(new Error(msg.error || 'Error de señalización'));
              }
            });
          });
          return null;
        }

        // HTTP fallback (also backed by Durable Object with 0ms stale cache)
        const pc = this.createPeerConnection();
        this.emitStatus(`Buscando sala ${this.room} en Worker...`, 'waiting');
        let offerSdp = null;
        for (let i = 0; i < 80 && !this.closed && !this.pollAborted; i++) {
          try {
            const res = await fetch(`${this.workerUrl}/room/${encodeURIComponent(this.room)}/offer`);
            if (res.status === 200) {
              const data = await res.json();
              if (data && data.sdp) {
                offerSdp = data.sdp;
                break;
              }
            }
          } catch {
            // Retry
          }
          await new Promise((r) => setTimeout(r, 300));
        }
        if (!offerSdp) throw new Error(`Sala ${this.room} no encontrada o expirada`);

        await pc.setRemoteDescription(offerSdp);
        const answer = await pc.createAnswer();
        await pc.setLocalDescription(answer);
        await waitIceComplete(pc, this.hasTurnServer(), 4000);
        const pRes = await fetch(`${this.workerUrl}/room/${encodeURIComponent(this.room)}/answer`, {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify({ sdp: pc.localDescription }),
        });
        if (!pRes.ok) throw new Error(`Error publicando Answer: HTTP ${pRes.status}`);
        this.emitStatus('Respuesta enviada. Negociando ruta P2P...', 'connecting');
        return null;
      }

      const pc = this.createPeerConnection();

      if (this.mode === 'manual') {
        const offerSdp = await decompressToken(manualOfferToken);
        await pc.setRemoteDescription(offerSdp);
        const answer = await pc.createAnswer();
        await pc.setLocalDescription(answer);
        await waitIceComplete(pc, this.hasTurnServer(), 4000);
        const ansToken = await compressToken(pc.localDescription);
        onManualAnswer?.(ansToken);
        this.emitStatus('Answer generada. Envíasela al Host.', 'waiting');
        return ansToken;
      }

      if (this.mode === 'local') {
        this.bc = new BroadcastChannel('twc98-rtc-room-v1');
        this.emitStatus(`Buscando sala local ${this.room}...`, 'waiting');
        let answered = false;
        this.bc.onmessage = async (ev) => {
          const m = ev.data || {};
          if (m.room !== this.room) return;
          if (m.t === 'host_offer' && m.sdp && !answered) {
            answered = true;
            await pc.setRemoteDescription(m.sdp);
            const answer = await pc.createAnswer();
            await pc.setLocalDescription(answer);
            await waitIceComplete(pc, false, 1500);
            const localAnswer = { type: pc.localDescription.type, sdp: pc.localDescription.sdp };
            this.bc.postMessage({ t: 'guest_answer', room: this.room, sdp: localAnswer });
            this.emitStatus('Answer enviada. Conectando P2P...', 'connecting');
          }
        };
        this.bc.postMessage({ t: 'guest_hello', room: this.room });
        return null;
      }
    }

    closeTransportOnly() {
      this.pollAborted = true;
      this.closeSignalingWs();
      if (this.disconnectTimer) { clearTimeout(this.disconnectTimer); this.disconnectTimer = null; }
      if (this.pingTimer) { clearInterval(this.pingTimer); this.pingTimer = null; }
      if (this.kbpsTimer) { clearInterval(this.kbpsTimer); this.kbpsTimer = null; }
      if (this.bc) { try { this.bc.close(); } catch {} this.bc = null; }
      if (this.dcInput) { try { this.dcInput.close(); } catch {} this.dcInput = null; }
      if (this.dcCtrl) { try { this.dcCtrl.close(); } catch {} this.dcCtrl = null; }
      if (this.pc) { try { this.pc.close(); } catch {} this.pc = null; }
    }

    close() {
      this.closed = true;
      this.closeTransportOnly();
    }
  }

  window.TwcRtcNet = {
    RtcP2PClient,
    compressToken,
    decompressToken,
    recommendDelayForRtt,
    prefetchWorkerIce,
  };
})();

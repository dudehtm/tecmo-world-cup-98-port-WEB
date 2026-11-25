(() => {
  const PREFS_KEY = 'twc98_prefs_v3';
  const LEGACY_PREFS_KEY = 'twc98_prefs_v2';
  const KEYMAP_P1_KEY = 'twc98_keymap_p1_v3';
  const KEYMAP_P2_KEY = 'twc98_keymap_p2_v3';
  const PADMAP_P1_KEY = 'twc98_padmap_p1_v1';
  const PADMAP_P2_KEY = 'twc98_padmap_p2_v1';
  const PADPREFS_KEY = 'twc98_padprefs_v1';

  const DEFAULT_P1_KEYMAP = {
    up: 'KeyW',
    down: 'KeyS',
    left: 'KeyA',
    right: 'KeyD',
    btn1: 'KeyJ',
    btn2: 'KeyK',
    btn3: 'KeyL',
    btn4: 'KeyI',
    start1: 'Space',
    coin1: 'KeyR',
  };

  const DEFAULT_P2_KEYMAP = {
    up: 'ArrowUp',
    down: 'ArrowDown',
    left: 'ArrowLeft',
    right: 'ArrowRight',
    btn1: 'Numpad1',
    btn2: 'Numpad2',
    btn3: 'Numpad3',
    btn4: 'Numpad5',
    start2: 'Digit0',
    coin2: 'Digit9',
  };

  const makeDefaultPadMap = (isP2) => ({
    up: { type: 'btn', index: 12, axis: 1, dir: -1 },
    down: { type: 'btn', index: 13, axis: 1, dir: 1 },
    left: { type: 'btn', index: 14, axis: 0, dir: -1 },
    right: { type: 'btn', index: 15, axis: 0, dir: 1 },
    btn1: { type: 'btn', index: 0 },
    btn2: { type: 'btn', index: 1 },
    btn3: { type: 'btn', index: 2 },
    btn4: { type: 'btn', index: 3 },
    ...(isP2
      ? { start2: { type: 'btn', index: 9 }, coin2: { type: 'btn', index: 8 } }
      : { start1: { type: 'btn', index: 9 }, coin1: { type: 'btn', index: 8 } }),
  });
  const DEFAULT_PADMAP_P1 = makeDefaultPadMap(false);
  const DEFAULT_PADMAP_P2 = makeDefaultPadMap(true);

  const I18N = {
    es: {
      pageTitle: "Tecmo World Cup '98 — Web + Online P2P",
      brandSub: 'EDICIÓN ARCADE · WEB & ONLINE P2P',
      langLabel: 'Idioma',
      langAuto: 'Automático (navegador)',
      discordBtn: 'Entra a nuestra comunidad',
      debugBtn: 'Debug',
      selectModeTitle: 'SELECT MODE',
      classicBanner: "CLASSIC '98",
      classicB1: 'Equipos oficiales',
      classicB2: 'Jugabilidad original',
      copaBanner: 'COPA AMÉRICA',
      copaB1: '12 selecciones',
      copaB2: 'Plantilla y poderes',
      startBtn: 'START',
      playOnlineBtn: 'Jugar Online 1v1',
      tournamentSoonBtn: 'Torneo Online (Pronto)',
      pausedOverlay: 'PAUSADO',
      loadingOverlay: 'CARGANDO...',
      tabMenu: 'Menú',
      tabOnline: 'Online',
      tabControls: 'Controles',
      sumGame: 'GAME',
      sumVideo: 'VIDEO',
      sumAudio: 'AUDIO',
      pauseBtn: 'Pausar',
      resumeBtn: 'Reanudar',
      restartBtn: 'Reiniciar',
      restartHostBtn: 'Reiniciar (Host)',
      fullscreenBtn: 'Pantalla completa',
      gameModeLbl: 'Modo de juego',
      onlineModeLbl: 'Modo de juego (Host)',
      hostLockHint: 'En partida Online, solo el Host controla Reiniciar y Modo de juego.',
      hardModeLbl: 'Modo Hard (Nivel Max)',
      hardModeHint: 'Dificultad máxima (Nivel 31) desde el juego 1',
      localCoopLbl: 'Cooperativo 2P (Local)',
      localCoopHint: 'P1 y P2 juntos en el mismo equipo vs CPU',
      onlineFormatLbl: 'Formato Online (Host)',
      onlineFmtVs: '1v1 Versus',
      onlineFmtCoop: '2P Cooperativo',
      onlineFormatHint: '1v1: P1 vs P2 · Cooperativo: ambos en el mismo equipo vs CPU (torneo incluido).',
      onlineRulesLbl: 'Reglas de partido (Host · pre-inicio)',
      onlineHardLbl: 'Modo Hard (Dificultad Máx)',
      onlineRulesLockHint: 'El Host configura estas reglas antes de iniciar; se bloquean en juego.',
      gkFixLbl: 'Portero IA (GK1 Fix)',
      gkFixHint: 'Corrección simétrica del portero',
      markerFixLbl: 'Indicador 1P/2P',
      markerFixHint: 'Muestra jugador al saque o fuera de cámara',
      touchPadLbl: 'Controles táctiles',
      touchPadHint: 'Joystick virtual + A/B/C en pantalla',
      touchAuto: 'AUTO',
      touchOn: 'ON',
      touchOff: 'OFF',
      vidHdModeLbl: 'Modo HD',
      vidHdModeHint: 'Ultra HD GPU: 2× 1408×896 RBG0 + RGSS + De-Mesh + FSR/FXAA (GPU dedicada)',
      vidFilterLbl: 'Filtro GPU',
      crtMaskLbl: 'Máscara CRT',
      crtMaskHint: 'Líneas y máscara tipo arcade',
      hd704Lbl: 'Cancha HD-704',
      hd704Hint: 'Alta resolución RBG0 (consume más CPU)',
      gpuVdp2Lbl: 'Aceleración GPU (VDP1+VDP2)',
      gpuVdp2Hint: 'Renderiza sprites VDP1 y capas 2D/3D VDP2 en GPU (60 FPS 448p)',
      lowRes224Lbl: 'Modo Fluidez 224p',
      lowRes224Hint: 'Mitad de líneas CPU (ideal laptops / iGPU / Online)',
      fpsModeLbl: 'FPS Objetivo',
      fpsModeHint: 'Auto baja a 30 FPS estables en gama baja sin perder velocidad de juego',
      fpsAutoLbl: 'Auto 60/30',
      lowPerfWarnMsg: 'Rendimiento bajo (<30 FPS): cierra otras pestañas o aplicaciones en segundo plano para mejorar la fluidez.',
      audioLiteBtn: 'Audio Lite (+30% FPS)',
      audioLiteOnBtn: 'Audio Lite: ON',
      aspectLbl: 'Relación de aspecto',
      audSoundLbl: 'Sonido',
      audVolLbl: 'Volumen',
      onlineTitle: 'ONLINE 1v1 P2P',
      roomCodeLbl: 'Código de Sala Privada',
      hostRoomBtn: 'Crear Sala (Host)',
      joinRoomBtn: 'Unirse (Guest)',
      startMatchBtn: 'Iniciar Partido',
      leaveOnlineBtn: 'Salir Online',
      sumOnlineAdv: 'Opciones de red avanzadas',
      sigModeLbl: 'Señalización',
      sigLocal: 'Local 2 Pestañas (sin server)',
      sigWorker: 'WebSocket Relay (internet)',
      sigManual: 'Manual Copy/Paste SDP',
      delayLbl: 'Input Delay',
      workerUrlLbl: 'URL Worker Señalización',
      applyTokenBtn: 'Aplicar Token',
      ctrlTopHint: 'Haz clic en una tecla para cambiarla. En Online juegas con Jugador 1 o Gamepad.',
      ctrlP1Title: 'JUGADOR 1 (WASD + J/K/L/I)',
      ctrlP2Title: 'JUGADOR 2 (Local · Flechas + Num 1/2/3/5)',
      resetDefaultBtn: 'Por defecto',
      pressKeyPrompt: 'Pulsa tecla...',
      pressPadPrompt: 'Pulsa botón...',
      padTwoOn: '2 Mandos: ON',
      padTwoOff: '2 Mandos: OFF',
      padSwapBtn: 'Invertir P1↔P2',
      padP1Title: 'MANDO 1 (P1)',
      padP2Title: 'MANDO 2 (P2)',
      gamepadConnected: 'Mandos detectados:',
      gamepadNone: 'Mando USB/Bluetooth: conecta cualquiera y pulsa un botón para configurarlo.',
      statusChoosing: 'Elige edición y pulsa START',
      statusPaused: 'Pausado',
      statusReady: 'Listo',
      statusOnlineLobby: 'Lobby Online',
      statusOnlineMatch: 'En partido Online',
      statusSwitching: 'Cargando edición...',
      statusRestarting: 'Reiniciando...',
      onlineOfflineMsg: 'Modo Offline activo. Crea una sala privada y comparte el código, o únete a una existente.',
      onlinePeerHostMsg: '¡Rival conectado! Pulsa "Iniciar Partido" para empezar en Frame 0.',
      onlinePeerGuestMsg: '¡Conectado al Host! El Host controla el Modo de juego, Inicio y Reinicio.',
      onlineSyncingMsg: 'Sincronizando Frame 0 con el rival...',
      onlinePrepMsg: 'Preparando partida Frame 0...',
      onlinePlayingHostMsg: 'Partido 1v1 en curso (Controlas P1).',
      onlinePlayingGuestMsg: 'Partido 1v1 en curso (Controlas P2).',
      onlinePlayingCoopHostMsg: 'Cooperativo 2P en curso (Eres 1P / Capitán · Mismo equipo vs CPU).',
      onlinePlayingCoopGuestMsg: 'Cooperativo 2P en curso (Eres 2P / Compañero · Mismo equipo vs CPU).',
      onlinePeerLeftMsg: 'El otro jugador salió de la sala.',
      onlineTokenErr: 'Pega primero el token Offer del Host y pulsa Unirse.',
      vaultTitle: "TECMO WORLD CUP '98",
      vaultPrivateTitle: 'TWC98BO — Acceso Privado',
      vaultChecking: 'Preparando el juego...',
      vaultDownloading: 'Descargando el juego...',
      vaultLoading: 'Cargando el juego...',
      vaultPassLbl: 'Código de acceso (o abrir enlace con #key=...)',
      vaultUnlock: 'Desbloquear y Jugar',
      vaultClear: 'Limpiar Caché Local',
      vaultEnterCode: 'Ingresa el código privado para desbloquear y jugar.',
      vaultInvalidCode: 'Código incorrecto o faltante. Verifica el enlace o escribe el código.',
      vaultCacheCleared: 'Caché local IndexedDB limpiada.',
      debugHotkeysNotice: '<strong>Atajos Debug (solo con Debug ON):</strong> F1(Edición) · F2/F3/F4(Capas) · F5/F8(Save/Load) · F6/.(+1f) · F7(QuickMatch) · F9/F10(Replay) · F11(Fullscreen) · F12(CRT) · B(Filtro) · Q(HD-704) · M(Mute) · 3/4 ó [/] (Velocidad) · Tab(Slot)',
      actionsP1: {
        up: 'Arriba', down: 'Abajo', left: 'Izquierda', right: 'Derecha',
        btn1: 'Pase / A', btn2: 'Tiro / B', btn3: 'Cambiar / Skill (C)', btn4: 'Pedir Pase / D',
        start1: '1P Start', coin1: 'Coin 1',
      },
      actionsP2: {
        up: 'Arriba', down: 'Abajo', left: 'Izquierda', right: 'Derecha',
        btn1: 'Pase / A', btn2: 'Tiro / B', btn3: 'Cambiar / Skill (C)', btn4: 'Pedir Pase / D',
        start2: '2P Start', coin2: 'Coin 2',
      },
    },
    en: {
      pageTitle: "Tecmo World Cup '98 — Web + Online P2P",
      brandSub: 'ARCADE EDITION · WEB & ONLINE P2P',
      langLabel: 'Language',
      langAuto: 'Auto (browser)',
      discordBtn: 'Join our community',
      debugBtn: 'Debug',
      selectModeTitle: 'SELECT MODE',
      classicBanner: "CLASSIC '98",
      classicB1: 'Official teams',
      classicB2: 'Original gameplay',
      copaBanner: 'COPA AMÉRICA',
      copaB1: '12 national teams',
      copaB2: 'New roster & powers',
      startBtn: 'START',
      playOnlineBtn: 'Play Online 1v1',
      tournamentSoonBtn: 'Online Tournament (Soon)',
      pausedOverlay: 'PAUSED',
      loadingOverlay: 'LOADING...',
      tabMenu: 'Menu',
      tabOnline: 'Online',
      tabControls: 'Controls',
      sumGame: 'GAME',
      sumVideo: 'VIDEO',
      sumAudio: 'AUDIO',
      pauseBtn: 'Pause',
      resumeBtn: 'Resume',
      restartBtn: 'Restart',
      restartHostBtn: 'Restart (Host)',
      fullscreenBtn: 'Fullscreen',
      gameModeLbl: 'Game Mode',
      onlineModeLbl: 'Game Mode (Host)',
      hostLockHint: 'In Online mode, only the Host can trigger Restart or switch Game Mode.',
      hardModeLbl: 'Hard Mode (Max AI)',
      hardModeHint: 'Maximum AI difficulty (Level 31) from Match 1',
      localCoopLbl: '2P Co-op (Local)',
      localCoopHint: 'P1 & P2 play together on the same team vs CPU',
      onlineFormatLbl: 'Online Format (Host)',
      onlineFmtVs: '1v1 Versus',
      onlineFmtCoop: '2P Co-op',
      onlineFormatHint: '1v1: P1 vs P2 · Co-op: both players on the same team vs CPU (full tournament).',
      onlineRulesLbl: 'Match Rules (Host · pre-start)',
      onlineHardLbl: 'Hard Mode (Max AI)',
      onlineRulesLockHint: 'Host configures rules before starting; locked while match is running.',
      gkFixLbl: 'Goalkeeper AI (GK1 Fix)',
      gkFixHint: 'Symmetric GK behavior fix',
      markerFixLbl: '1P/2P Indicator',
      markerFixHint: 'Show player at kickoff or off-screen',
      touchPadLbl: 'Touch Controls',
      touchPadHint: 'Virtual joystick + on-screen A/B/C pad',
      touchAuto: 'AUTO',
      touchOn: 'ON',
      touchOff: 'OFF',
      vidHdModeLbl: 'HD Mode',
      vidHdModeHint: 'Ultra HD GPU: 2× 1408×896 RBG0 + RGSS + De-Mesh + FSR/FXAA (dedicated GPU)',
      vidFilterLbl: 'GPU Filter',
      crtMaskLbl: 'CRT Mask',
      crtMaskHint: 'Arcade scanlines & shadow mask',
      hd704Lbl: 'HD-704 Pitch',
      hd704Hint: 'High-res RBG0 (higher CPU cost)',
      gpuVdp2Lbl: 'GPU Acceleration (VDP1+VDP2)',
      gpuVdp2Hint: 'Renders VDP1 sprites & VDP2 2D/3D layers on GPU (60 FPS 448p)',
      lowRes224Lbl: 'Smooth 224p Mode',
      lowRes224Hint: 'Half CPU scanlines (ideal for laptops / iGPU / Online)',
      fpsModeLbl: 'Target FPS',
      fpsModeHint: 'Auto steps down to steady 30 FPS on low-end devices at full game speed',
      fpsAutoLbl: 'Auto 60/30',
      lowPerfWarnMsg: 'Low performance (<30 FPS): close background tabs or apps for smoother gameplay.',
      audioLiteBtn: 'Audio Lite (+30% FPS)',
      audioLiteOnBtn: 'Audio Lite: ON',
      aspectLbl: 'Aspect Ratio',
      audSoundLbl: 'Audio',
      audVolLbl: 'Volume',
      onlineTitle: 'ONLINE 1v1 P2P',
      roomCodeLbl: 'Private Room Code',
      hostRoomBtn: 'Create Room (Host)',
      joinRoomBtn: 'Join Room (Guest)',
      startMatchBtn: 'Start Match',
      leaveOnlineBtn: 'Leave Online',
      sumOnlineAdv: 'Advanced network settings',
      sigModeLbl: 'Signaling',
      sigLocal: 'Local 2 Tabs (no server)',
      sigWorker: 'WebSocket Relay (internet)',
      sigManual: 'Manual Copy/Paste SDP',
      delayLbl: 'Input Delay',
      workerUrlLbl: 'Worker Signaling URL',
      applyTokenBtn: 'Apply Token',
      ctrlTopHint: 'Click any key to rebind. Online uses Player 1 keys or Gamepad.',
      ctrlP1Title: 'PLAYER 1 (WASD + J/K/L/I)',
      ctrlP2Title: 'PLAYER 2 (Local · Arrows + Num 1/2/3/5)',
      resetDefaultBtn: 'Reset Defaults',
      pressKeyPrompt: 'Press key...',
      pressPadPrompt: 'Press button...',
      padTwoOn: '2 Gamepads: ON',
      padTwoOff: '2 Gamepads: OFF',
      padSwapBtn: 'Swap P1↔P2',
      padP1Title: 'GAMEPAD 1 (P1)',
      padP2Title: 'GAMEPAD 2 (P2)',
      gamepadConnected: 'Detected gamepads:',
      gamepadNone: 'USB/Bluetooth Gamepad: connect a controller and press any button to configure it.',
      statusChoosing: 'Select edition and press START',
      statusPaused: 'Paused',
      statusReady: 'Ready',
      statusOnlineLobby: 'Online Lobby',
      statusOnlineMatch: 'Online Match',
      statusSwitching: 'Loading edition...',
      statusRestarting: 'Restarting...',
      onlineOfflineMsg: 'Offline mode. Create a private room and share the code, or join an existing room.',
      onlinePeerHostMsg: 'Opponent connected! Press "Start Match" to begin at Frame 0.',
      onlinePeerGuestMsg: 'Connected to Host! The Host controls Game Mode, Start and Restart.',
      onlineSyncingMsg: 'Syncing Frame 0 with opponent...',
      onlinePrepMsg: 'Preparing Frame 0 match state...',
      onlinePlayingHostMsg: '1v1 Match running (You control P1).',
      onlinePlayingGuestMsg: '1v1 Match running (You control P2).',
      onlinePlayingCoopHostMsg: '2P Co-op running (You are 1P / Captain · Same team vs CPU).',
      onlinePlayingCoopGuestMsg: '2P Co-op running (You are 2P / Partner · Same team vs CPU).',
      onlinePeerLeftMsg: 'The other player left the room.',
      onlineTokenErr: 'Paste the Host Offer token first, then click Join.',
      vaultTitle: "TECMO WORLD CUP '98",
      vaultPrivateTitle: 'TWC98BO — Private Access',
      vaultChecking: 'Preparing game...',
      vaultDownloading: 'Downloading game...',
      vaultLoading: 'Loading game...',
      vaultPassLbl: 'Passcode (or open link with #key=...)',
      vaultUnlock: 'Unlock & Play',
      vaultClear: 'Clear Local Cache',
      vaultEnterCode: 'Enter your private passcode to unlock and play.',
      vaultInvalidCode: 'Invalid or missing passcode. Check the link or re-enter.',
      vaultCacheCleared: 'IndexedDB local cache cleared.',
      debugHotkeysNotice: '<strong>Debug Hotkeys (active only when Debug is ON):</strong> F1(Edition) · F2/F3/F4(Layers) · F5/F8(Save/Load) · F6/.(+1f) · F7(QuickMatch) · F9/F10(Replay) · F11(Fullscreen) · F12(CRT) · B(Filter) · Q(HD-704) · M(Mute) · 3/4 or [/] (Speed) · Tab(Slot)',
      actionsP1: {
        up: 'Up', down: 'Down', left: 'Left', right: 'Right',
        btn1: 'Pass / A', btn2: 'Shoot / B', btn3: 'Switch / Skill (C)', btn4: 'Call Pass / D',
        start1: '1P Start', coin1: 'Coin 1',
      },
      actionsP2: {
        up: 'Up', down: 'Down', left: 'Left', right: 'Right',
        btn1: 'Pass / A', btn2: 'Shoot / B', btn3: 'Switch / Skill (C)', btn4: 'Call Pass / D',
        start2: '2P Start', coin2: 'Coin 2',
      },
    },
  };

  const P1_ACTION_KEYS = ['up', 'down', 'left', 'right', 'btn1', 'btn2', 'btn3', 'btn4', 'start1', 'coin1'];
  const P2_ACTION_KEYS = ['up', 'down', 'left', 'right', 'btn1', 'btn2', 'btn3', 'btn4', 'start2', 'coin2'];
  const FILTER_NAMES = ['Nearest', 'Bilinear', 'Sharp-Bi', 'CRT-Arcade', 'Suave xBR'];
  const SPEED_PERCENTS = [25, 50, 100, 200];

  // DOM References
  const editionPill = document.getElementById('editionPill');
  const brandSub = document.getElementById('brandSub');
  const lblDiscordCommunity = document.getElementById('lblDiscordCommunity');
  const lblLang = document.getElementById('lblLang');
  const selLang = document.getElementById('selLang');
  const btnDebugToggle = document.getElementById('btnDebugToggle');

  const initQs = new URLSearchParams(window.location.search);
  const initHs = new URLSearchParams((window.location.hash || '').slice(1));
  const profEnabled = initQs.get('prof') === '1';
  const idleSkipEnabled = initQs.get('idle') !== '0';
  const sh2AotEnabled = initQs.get('aot') !== '0';
  const fastCpuEnabled = initQs.get('fast_cpu') !== '0';
  if (initQs.get('touch_blur') === '1' && document.body) {
    document.body.classList.add('touch-blur-legacy');
  }

  const screenWrap = document.getElementById('screenWrap');
  const canvas = document.getElementById('screen');
  const detectedMobile = (window.TwcPerfProfile && typeof window.TwcPerfProfile.detectMobileDevice === 'function')
    ? window.TwcPerfProfile.detectMobileDevice()
    : (/Android|iPhone|iPad|iPod|Mobile/i.test(navigator.userAgent || '') ||
      (window.matchMedia && window.matchMedia('(pointer: coarse)').matches && window.innerWidth <= 1100));
  const gpuVdpParam = initQs.get('gpu_vdp'); // '0' | '1' | null (default: auto-ON on real WebGL2 GPU)
  const wantGpuVdp = (gpuVdpParam !== '0');
  const forceSoftwareGpuVdp = (gpuVdpParam === '1');
  const gpuVdp1Param = initQs.get('gpu_vdp1'); // '0' | '1' | null (default: auto-ON when gpuVdpActive)
  const wantGpuVdp1 = (gpuVdp1Param !== '0');
  const forceScanlineVdp1 = (initQs.get('v1_scanline') === '1');
  const fpsModeParam = initQs.get('fps'); // 'auto' | '60' | '30' | null
  const bandsParam = initQs.get('bands'); // '0' | '2' | '3' | '4' | null
  const mtParam = initQs.get('mt');
  const wantMt = !wantGpuVdp && (mtParam === '1');
  let detectedSoftwareGpu = false;
  let detectedIntegratedGpu = false;
  let canOffscreenWebGL2 = false;
  if (wantMt && typeof SharedArrayBuffer !== 'undefined' &&
      typeof OffscreenCanvas !== 'undefined' && typeof canvas.transferControlToOffscreen === 'function') {
    try {
      const probe = new OffscreenCanvas(1, 1);
      const pgl = probe.getContext('webgl2', { alpha: false, antialias: false });
      if (pgl) {
        canOffscreenWebGL2 = true;
        try {
          const dbgExt = pgl.getExtension('WEBGL_debug_renderer_info');
          const rStr = dbgExt ? String(pgl.getParameter(dbgExt.UNMASKED_RENDERER_WEBGL) || '') : '';
          if (/SwiftShader|llvmpipe|Microsoft Basic Render|Software/i.test(rStr)) {
            detectedSoftwareGpu = true;
          } else if (!detectedMobile && /Intel|UHD Graphics|Iris|HD Graphics|Radeon.*Graphics|Vega|Apple GPU/i.test(rStr) && !/NVIDIA|GeForce|RTX|GTX|Radeon RX|Quadro|Arc\s*A/i.test(rStr)) {
            detectedIntegratedGpu = true;
          }
        } catch {}
        try { pgl.getExtension('WEBGL_lose_context')?.loseContext(); } catch {}
      }
    } catch {}
  }

  let renderWorker = null;
  let renderWorkerVisFps = 0;
  let renderWorkerFrames = 0;
  let mtActive = wantMt && canOffscreenWebGL2;
  const RENDER_SLOT_BYTES = 2304000;

  // Phase 2: Multi-Core CPU Band Striping Pool constants & state
  const BAND_PKT_OFFSET = 64;
  const BAND_PKT_BYTES = 1638400; // 1600 KiB ("URND" v2 <= 1413 KiB)
  const BAND_RGBA_STRIDE = 704 * 448 * 4;
  const BAND_RGBA_OFFSET0 = BAND_PKT_OFFSET + BAND_PKT_BYTES;
  const BAND_SAB_BYTES = BAND_RGBA_OFFSET0 + 2 * BAND_RGBA_STRIDE;
  let bandWorkers = [];
  let bandSab = null;
  let bandSabU8 = null;
  let bandHdr = null;
  let bandPoolActive = false;
  let bandDisplayBuf = null;
  let pendingBandFrame = null;
  let cachedWasmBytesForBands = null;

  function computeCanvasTargetSize() {
    const rect = canvas.getBoundingClientRect();
    const isHd = (typeof prefs !== 'undefined' && Boolean(prefs.hdMode));
    const maxDpr = detectedSoftwareGpu ? 1 : (isHd ? ((detectedMobile || detectedIntegratedGpu) ? 2.0 : 2.5) : ((detectedMobile || detectedIntegratedGpu) ? 1.5 : 2));
    const dpr = Math.min(maxDpr, window.devicePixelRatio || 1);
    const minW = (detectedMobile || detectedSoftwareGpu) ? 352 : 704;
    const minH = (detectedMobile || detectedSoftwareGpu) ? 224 : 448;
    return {
      w: Math.max(minW, Math.round(rect.width * dpr)),
      h: Math.max(minH, Math.round(rect.height * dpr)),
    };
  }

  let presenter = mtActive ? {
    backendName: 'WebGL2-Worker',
    isMobile: detectedMobile,
    isSoftwareRenderer: detectedSoftwareGpu,
    isIntegratedGpu: detectedIntegratedGpu,
    filterMode: 3,
    crtMask: true,
    hdMode: false,
    useGpu: true,
    hasGpuVdp2() { return false; },
    hasGpuVdp1() { return false; },
    repaintLast() { return false; },
    setFilterMode(m) {
      this.filterMode = Math.max(0, Math.min(4, m | 0));
      if (renderWorker) renderWorker.postMessage({ type: 'video_prefs', filterMode: this.filterMode });
    },
    setCrtMask(e) {
      this.crtMask = Boolean(e);
      if (renderWorker) renderWorker.postMessage({ type: 'video_prefs', crtMask: this.crtMask });
    },
    setHdMode(e) {
      this.hdMode = Boolean(e);
      if (renderWorker) renderWorker.postMessage({ type: 'video_prefs', hdMode: this.hdMode });
      this.invalidateSize();
    },
    setUseGpu(e) {
      this.useGpu = Boolean(e);
      if (renderWorker) renderWorker.postMessage({ type: 'video_prefs', useGpu: this.useGpu });
    },
    invalidateSize() {
      if (renderWorker) {
        const sz = computeCanvasTargetSize();
        renderWorker.postMessage({ type: 'resize', w: sz.w, h: sz.h });
      }
    },
    present() {},
  } : new window.TwcWebGLPresenter(canvas, { allowSoftwareVdp2: forceSoftwareGpuVdp, forceScanlineVdp1 });

  const gpuVdpSupported = Boolean(presenter && typeof presenter.hasGpuVdp2 === 'function' && presenter.hasGpuVdp2());
  let gpuVdpActive = wantGpuVdp && gpuVdpSupported;
  const gpuVdp1Supported = Boolean(gpuVdpSupported && typeof presenter.hasGpuVdp1 === 'function' && presenter.hasGpuVdp1());
  let gpuVdp1Active = gpuVdpActive && wantGpuVdp1 && gpuVdp1Supported;
  const deviceProfile = (window.TwcPerfProfile && typeof window.TwcPerfProfile.detectDeviceProfile === 'function')
    ? window.TwcPerfProfile.detectDeviceProfile({
      isMobile: detectedMobile,
      isSoftwareRenderer: Boolean(presenter && presenter.isSoftwareRenderer),
      isIntegratedGpu: Boolean(presenter && presenter.isIntegratedGpu),
      gpuVdpActive,
    })
    : { isMobile: detectedMobile, cores: navigator.hardwareConcurrency || 4, deviceMemoryGb: 4, isLowEnd: detectedMobile };

  const audioEngine = new window.TwcAudioEngine({
    isMobile: detectedMobile,
    onSabModeChange: (useSab) => {
      if (worker) worker.postMessage({ type: 'set_audio_sab', enabled: Boolean(useSab) });
    },
  });

  const accessOverlay = document.getElementById('accessOverlay');
  const accessTitle = document.getElementById('accessTitle');
  const accessSub = document.getElementById('accessSub');
  const accessFormWrap = document.getElementById('accessFormWrap');
  const lblAccessPasscode = document.getElementById('lblAccessPasscode');
  const inputPasscode = document.getElementById('inputPasscode');
  const btnUnlockVault = document.getElementById('btnUnlockVault');
  const btnClearVaultCache = document.getElementById('btnClearVaultCache');
  const accessBar = document.getElementById('accessBar');
  const accessStatus = document.getElementById('accessStatus');

  const modeSelectOverlay = document.getElementById('modeSelectOverlay');
  const modeSelectTitle = document.getElementById('modeSelectTitle');
  const cardClassicMode = document.getElementById('cardClassicMode');
  const cardCopaMode = document.getElementById('cardCopaMode');
  const liClassic1 = document.getElementById('liClassic1');
  const liClassic2 = document.getElementById('liClassic2');
  const liCopa1 = document.getElementById('liCopa1');
  const liCopa2 = document.getElementById('liCopa2');
  const btnStartSelectedMode = document.getElementById('btnStartSelectedMode');
  const btnStartPlayOnline = document.getElementById('btnStartPlayOnline');
  const btnStartTournamentSoon = document.getElementById('btnStartTournamentSoon');

  const netPill = document.getElementById('netPill');
  const btnScreenPause = document.getElementById('btnScreenPause');
  const pauseBanner = document.getElementById('pauseBanner');
  const pauseBannerText = document.getElementById('pauseBannerText');
  const statusDot = document.getElementById('statusDot');
  const stageStatusText = document.getElementById('stageStatusText');
  const stageVersionTag = document.getElementById('stageVersionTag');
  const stageFpsClean = document.getElementById('stageFpsClean');
  const profBar = document.getElementById('profBar');
  const fsProfBar = document.getElementById('fsProfBar');
  if (profBar && profEnabled) profBar.classList.remove('hidden');
  if (fsProfBar && profEnabled) fsProfBar.classList.remove('hidden');
  const lowPerfBanner = document.getElementById('lowPerfBanner');
  const lowPerfBannerText = document.getElementById('lowPerfBannerText');
  const btnLowPerfAudioLite = document.getElementById('btnLowPerfAudioLite');
  const btnLowPerfDismiss = document.getElementById('btnLowPerfDismiss');
  let lowPerfDismissed = false;
  let pacerState = { mode: 'auto', renderEvery: 1, targetVisFps: 60, below30Warning: false, simEma: 0 };

  const btnCoin1Footer = document.getElementById('btnCoin1');
  const btnStart1Footer = document.getElementById('btnStart1');
  const btnFullscreenFooter = document.getElementById('btnFullscreenFooter');
  const btnRestartFooter = document.getElementById('btnRestartFooter');

  // Right Sidebar Tabs & Panes
  const mainTabButtons = [
    document.getElementById('mainTabMenu'),
    document.getElementById('mainTabOnline'),
    document.getElementById('mainTabControls'),
  ];
  const mainTabPanes = [
    document.getElementById('paneMainMenu'),
    document.getElementById('paneMainOnline'),
    document.getElementById('paneMainControls'),
  ];

  // Menú Accordions
  const sumGame = document.getElementById('sumGame');
  const sumVideo = document.getElementById('sumVideo');
  const sumAudio = document.getElementById('sumAudio');
  const btnMenuPause = document.getElementById('btnMenuPause');
  const btnMenuRestart = document.getElementById('btnMenuRestart');
  const lblMenuMode = document.getElementById('lblMenuMode');
  const btnMenuEdClassic = document.getElementById('btnMenuEdClassic');
  const btnMenuEdCopa = document.getElementById('btnMenuEdCopa');
  const hintMenuHostLock = document.getElementById('hintMenuHostLock');
  const lblMenuHardMode = document.getElementById('lblMenuHardMode');
  const hintMenuHardMode = document.getElementById('hintMenuHardMode');
  const btnMenuHardMode = document.getElementById('btnMenuHardMode');
  const lblMenuLocalCoop = document.getElementById('lblMenuLocalCoop');
  const hintMenuLocalCoop = document.getElementById('hintMenuLocalCoop');
  const btnMenuLocalCoop = document.getElementById('btnMenuLocalCoop');
  const lblMenuTouchPad = document.getElementById('lblMenuTouchPad');
  const hintMenuTouchPad = document.getElementById('hintMenuTouchPad');
  const btnMenuTouchPad = document.getElementById('btnMenuTouchPad');

  const touchControlsOverlay = document.getElementById('touchControlsOverlay');
  const tBtnCoin = document.getElementById('tBtnCoin');
  const tBtnStart = document.getElementById('tBtnStart');
  const tJoyZone = document.getElementById('tJoyZone');
  const tJoyAnchor = document.getElementById('tJoyAnchor');
  const tJoyKnob = document.getElementById('tJoyKnob');
  const tAbcPad = document.getElementById('tAbcPad');
  const tRouteAB = document.getElementById('tRouteAB');
  const tRouteAC = document.getElementById('tRouteAC');
  const tRouteBC = document.getElementById('tRouteBC');
  const tPillAB = document.getElementById('tPillAB');
  const tPillAC = document.getElementById('tPillAC');
  const tPillBC = document.getElementById('tPillBC');
  const tBtnA = document.getElementById('tBtnA');
  const tBtnB = document.getElementById('tBtnB');
  const tBtnC = document.getElementById('tBtnC');
  const tBtnAbc = document.getElementById('tBtnAbc');

  const lblVidHdMode = document.getElementById('lblVidHdMode');
  const hintVidHdMode = document.getElementById('hintVidHdMode');
  const btnVidHdMode = document.getElementById('btnVidHdMode');
  const lblVidFilter = document.getElementById('lblVidFilter');
  const filterButtons = [
    document.getElementById('btnVidFilter0'),
    document.getElementById('btnVidFilter1'),
    document.getElementById('btnVidFilter2'),
    document.getElementById('btnVidFilter3'),
    document.getElementById('btnVidFilter4'),
  ];
  const lblVidCrt = document.getElementById('lblVidCrt');
  const hintVidCrt = document.getElementById('hintVidCrt');
  const btnVidCrtMask = document.getElementById('btnVidCrtMask');
  const lblVidHd = document.getElementById('lblVidHd');
  const hintVidHd = document.getElementById('hintVidHd');
  const btnVidHd704 = document.getElementById('btnVidHd704');
  const lblVidGpuVdp2 = document.getElementById('lblVidGpuVdp2');
  const hintVidGpuVdp2 = document.getElementById('hintVidGpuVdp2');
  const btnVidGpuVdp2 = document.getElementById('btnVidGpuVdp2');
  const lblVidLowRes224 = document.getElementById('lblVidLowRes224');
  const hintVidLowRes224 = document.getElementById('hintVidLowRes224');
  const btnVidLowRes224 = document.getElementById('btnVidLowRes224');
  const lblVidFpsMode = document.getElementById('lblVidFpsMode');
  const hintVidFpsMode = document.getElementById('hintVidFpsMode');
  const btnVidFpsAuto = document.getElementById('btnVidFpsAuto');
  const btnVidFps60 = document.getElementById('btnVidFps60');
  const btnVidFps30 = document.getElementById('btnVidFps30');
  const lblVidAspect = document.getElementById('lblVidAspect');
  const btnVidAspectFit = document.getElementById('btnVidAspectFit');
  const btnVidAspect43 = document.getElementById('btnVidAspect43');
  const btnVidAspectStretch = document.getElementById('btnVidAspectStretch');
  const btnVidFullscreen = document.getElementById('btnVidFullscreen');

  const lblAudMute = document.getElementById('lblAudMute');
  const btnAudMute = document.getElementById('btnAudMute');
  const lblAudVol = document.getElementById('lblAudVol');
  const lblVolumePct = document.getElementById('lblVolumePct');
  const rngVolume = document.getElementById('rngVolume');

  // Online Tab Controls
  const lblOnlineTitle = document.getElementById('lblOnlineTitle');
  const onlineRoleTag = document.getElementById('onlineRoleTag');
  const lblOnlineMode = document.getElementById('lblOnlineMode');
  const btnOnlineEdClassic = document.getElementById('btnOnlineEdClassic');
  const btnOnlineEdCopa = document.getElementById('btnOnlineEdCopa');
  const lblOnlineFormat = document.getElementById('lblOnlineFormat');
  const btnOnlineFmtVs = document.getElementById('btnOnlineFmtVs');
  const btnOnlineFmtCoop = document.getElementById('btnOnlineFmtCoop');
  const hintOnlineFormat = document.getElementById('hintOnlineFormat');
  const lblOnlineRules = document.getElementById('lblOnlineRules');
  const selOnlineRule = document.getElementById('selOnlineRule');
  const lblOnlineHardMode = document.getElementById('lblOnlineHardMode');
  const btnOnlineHardMode = document.getElementById('btnOnlineHardMode');
  const hintOnlineRulesLock = document.getElementById('hintOnlineRulesLock');
  const lblRoomCode = document.getElementById('lblRoomCode');
  const inputRoomCode = document.getElementById('inputRoomCode');
  const btnCopyInvite = document.getElementById('btnCopyInvite');
  const btnHostRoom = document.getElementById('btnHostRoom');
  const btnJoinRoom = document.getElementById('btnJoinRoom');
  const btnStartOnlineMatch = document.getElementById('btnStartOnlineMatch');
  const btnOnlineRestart = document.getElementById('btnOnlineRestart');
  const btnLeaveOnline = document.getElementById('btnLeaveOnline');
  const onlineStatusBox = document.getElementById('onlineStatusBox');
  const sumOnlineAdv = document.getElementById('sumOnlineAdv');
  const lblSigMode = document.getElementById('lblSigMode');
  const selSigMode = document.getElementById('selSigMode');
  const optSigLocal = document.getElementById('optSigLocal');
  const lblNetDelay = document.getElementById('lblNetDelay');
  const selNetDelay = document.getElementById('selNetDelay');
  const lblWorkerUrl = document.getElementById('lblWorkerUrl');
  const workerUrlWrap = document.getElementById('workerUrlWrap');
  const turnConfigBlock = document.getElementById('turnConfigBlock');
  const inputWorkerUrl = document.getElementById('inputWorkerUrl');
  const inputTurnUrl = document.getElementById('inputTurnUrl');
  const inputTurnUser = document.getElementById('inputTurnUser');
  const inputTurnCred = document.getElementById('inputTurnCred');
  const manualSdpBox = document.getElementById('manualSdpBox');
  const txtManualOut = document.getElementById('txtManualOut');
  const txtManualIn = document.getElementById('txtManualIn');
  const btnApplyManualToken = document.getElementById('btnApplyManualToken');

  // Controls Tab
  const hintControlsTop = document.getElementById('hintControlsTop');
  const lblCtrlP1Title = document.getElementById('lblCtrlP1Title');
  const lblCtrlP2Title = document.getElementById('lblCtrlP2Title');
  const controlsRemapP1 = document.getElementById('controlsRemapP1');
  const controlsRemapP2 = document.getElementById('controlsRemapP2');
  const btnResetP1 = document.getElementById('btnResetP1');
  const btnResetP2 = document.getElementById('btnResetP2');
  const menuGamepadInfo = document.getElementById('menuGamepadInfo');
  const gamepadConfigWrap = document.getElementById('gamepadConfigWrap');
  const btnPadTwoMode = document.getElementById('btnPadTwoMode');
  const btnPadSwap = document.getElementById('btnPadSwap');
  const padCardP1 = document.getElementById('padCardP1');
  const padCardP2 = document.getElementById('padCardP2');
  const lblPadP1Title = document.getElementById('lblPadP1Title');
  const lblPadP2Title = document.getElementById('lblPadP2Title');
  const btnResetPad1 = document.getElementById('btnResetPad1');
  const btnResetPad2 = document.getElementById('btnResetPad2');
  const controlsRemapPad1 = document.getElementById('controlsRemapPad1');
  const controlsRemapPad2 = document.getElementById('controlsRemapPad2');

  // Temporary Debug Drawer (Hidden by default)
  const debugPanel = document.getElementById('debugPanel');
  const controlsDocBar = document.getElementById('controlsDocBar');
  const btnRec = document.getElementById('btnRec');
  const btnPlayReplay = document.getElementById('btnPlayReplay');
  const chkHideVdp1 = document.getElementById('chkHideVdp1');
  const chkHideRbg0 = document.getElementById('chkHideRbg0');
  const chkHideNbg = document.getElementById('chkHideNbg');
  const chkHd704 = document.getElementById('chkHd704');
  const btnVidUseGpu = document.getElementById('btnVidUseGpu');
  const btnAudRealScsp = document.getElementById('btnAudRealScsp');
  const menuVideoBackend = document.getElementById('menuVideoBackend');
  const menuAudioInfo = document.getElementById('menuAudioInfo');

  const speedButtons = [
    document.getElementById('btnSpd25'),
    document.getElementById('btnSpd50'),
    document.getElementById('btnSpd100'),
    document.getElementById('btnSpd200'),
  ];
  const slotButtons = [
    document.getElementById('btnSlot0'),
    document.getElementById('btnSlot1'),
    document.getElementById('btnSlot2'),
  ];

  const elCold = document.getElementById('stColdInit');
  const elFps = document.getElementById('stFps');
  const elFrames = document.getElementById('stFrames');
  const elPcs = document.getElementById('stPcs');
  const elMem = document.getElementById('stMem');
  const elNetHud = document.getElementById('stNetHud');
  const elSyncHash = document.getElementById('stSyncHash');
  const elScsp = document.getElementById('stScsp');
  const elCart = document.getElementById('stCart');
  const elSave = document.getElementById('stSave');
  const elCanvasSha = document.getElementById('stCanvasSha');
  const elBench = document.getElementById('stBench');
  const elReplayInfo = document.getElementById('stReplayInfo');
  const elScspLog = document.getElementById('stScspLog');

  function loadJsonPrefs(key, fallback) {
    try {
      const raw = localStorage.getItem(key);
      if (!raw) return { ...fallback };
      return { ...fallback, ...JSON.parse(raw) };
    } catch {
      return { ...fallback };
    }
  }

  function saveJsonPrefs(key, obj) {
    try { localStorage.setItem(key, JSON.stringify(obj)); } catch {}
  }

  function loadPrefsWithMigration(fallback) {
    try {
      const raw = localStorage.getItem(PREFS_KEY);
      if (raw) return { ...fallback, ...JSON.parse(raw) };
      const legacy = localStorage.getItem(LEGACY_PREFS_KEY);
      if (legacy) {
        const parsed = JSON.parse(legacy);
        // Classic '98 is default at startup; HD-704 stays off by default for 60 FPS
        return {
          ...fallback,
          filterMode: parsed.filterMode ?? fallback.filterMode,
          crtMask: parsed.crtMask ?? fallback.crtMask,
          aspect: parsed.aspect ?? fallback.aspect,
          useGpu: parsed.useGpu ?? fallback.useGpu,
          volumePct: parsed.volumePct ?? fallback.volumePct,
          muted: parsed.muted ?? fallback.muted,
          realScsp: parsed.realScsp ?? fallback.realScsp,
          hdMode: false,
    hd704: false,
          savedCopaMode: false,
        };
      }
    } catch {}
    return { ...fallback };
  }

  const isLowEndGpuOrMobile = Boolean(presenter && (presenter.isMobile || presenter.isSoftwareRenderer));
  if (presenter && presenter.isSoftwareRenderer) detectedSoftwareGpu = true;
  if (presenter && presenter.isIntegratedGpu) detectedIntegratedGpu = true;
  function loadMobileSharpPrefs(fallback) {
    const loaded = loadPrefsWithMigration(fallback);
    if (detectedMobile && !loaded.mobileLiteV2Migrated) {
      // Lightweight mobile GPU defaults: 1-tap Linear filter + no CRT mask (when HD is off)
      if (!loaded.hdMode) {
        loaded.filterMode = 1;
        loaded.crtMask = false;
      }
      loaded.mobileLiteV2Migrated = true;
      saveJsonPrefs(PREFS_KEY, loaded);
    }
    if (detectedMobile && gpuVdpActive && !loaded.mobile448GpuMigrated) {
      // With WebGL2 UBO + zero-blur compositor, full 448p costs <1 ms GPU on low-end mobile
      loaded.lowRes224p = false;
      loaded.mobile448GpuMigrated = true;
      saveJsonPrefs(PREFS_KEY, loaded);
    }
    let hasSavedLowRes = false;
    try {
      const raw = localStorage.getItem(PREFS_KEY);
      if (raw) {
        const parsed = JSON.parse(raw);
        hasSavedLowRes = Boolean(parsed && typeof parsed.lowRes224p === 'boolean');
      }
    } catch {}
    const renderParam = initQs.get('render');
    const defaultLowRes = gpuVdpActive ? false : Boolean(isLowEndGpuOrMobile || detectedIntegratedGpu);
    if (renderParam === '224') {
      loaded.lowRes224p = true;
    } else if (renderParam === '448') {
      loaded.lowRes224p = false;
    } else if (loaded.lowRes224p === undefined || loaded.lowRes224p === null) {
      loaded.lowRes224p = loaded.hdMode ? false : defaultLowRes;
    } else if (!hasSavedLowRes && (loaded.hdMode || gpuVdpActive)) {
      loaded.lowRes224p = false;
    }
    if (fpsModeParam) {
      loaded.fpsMode = (window.TwcPerfProfile && window.TwcPerfProfile.normalizeFpsMode)
        ? window.TwcPerfProfile.normalizeFpsMode(fpsModeParam)
        : 'auto';
    } else if (!loaded.fpsMode) {
      loaded.fpsMode = 'auto';
    }
    return loaded;
  }
  const prefs = loadMobileSharpPrefs({
    lang: 'auto',
    filterMode: (detectedSoftwareGpu || isLowEndGpuOrMobile) ? 1 : 3,
    crtMask: !isLowEndGpuOrMobile,
    hdMode: false,
    hd704: false,
    lowRes224p: gpuVdpActive ? false : Boolean(isLowEndGpuOrMobile || detectedIntegratedGpu),
    fpsMode: 'auto',
    aspect: '4:3',
    useGpu: true,
    volumePct: 100,
    muted: false,
    realScsp: true,
    savedCopaMode: false,
    touchPad: 'auto',
    sharpMobileMigrated: true,
    mobileLiteV2Migrated: true,
    mobile448GpuMigrated: true,
  });

  let p1KeyMap = loadJsonPrefs(KEYMAP_P1_KEY, DEFAULT_P1_KEYMAP);
  let p2KeyMap = loadJsonPrefs(KEYMAP_P2_KEY, DEFAULT_P2_KEYMAP);
  let p1PadMap = loadJsonPrefs(PADMAP_P1_KEY, DEFAULT_PADMAP_P1);
  let p2PadMap = loadJsonPrefs(PADMAP_P2_KEY, DEFAULT_PADMAP_P2);
  let padPrefs = loadJsonPrefs(PADPREFS_KEY, { twoPads: false, swapPads: false });
  let listeningTarget = null; // { player: 1 | 2, action: string }
  let listeningPadTarget = null; // { player: 1 | 2, action: string, padIdx: number }
  let padListenBaseline = null;
  let lastPadDetectKey = '';

  function resolveLangCode() {
    if (prefs.lang === 'es' || prefs.lang === 'en') return prefs.lang;
    const nav = ((navigator.language || 'es') + '').toLowerCase();
    return nav.startsWith('en') ? 'en' : 'es';
  }

  function t(key) {
    const lang = resolveLangCode();
    return (I18N[lang] && I18N[lang][key] !== undefined) ? I18N[lang][key] : (I18N.es[key] || key);
  }

  const stats = {
    ready: false,
    error: null,
    initMode: '-',
    coldInitMs: 0,
    framesRun: 0,
    fps: 0,
    avgMs: 0,
    p50Ms: 0,
    p95Ms: 0,
    wasmBinaryBytes: 0,
    wasmLinearMemoryBytes: 0,
    wasmMemMB: 0,
    jsHeapMB: null,
    halted: false,
    iogaReads: 0,
    cartReads: 0,
    soundramWrites: 0,
    scspEvents: 0,
    audioEventsTriggered: 0,
    m68kEnabled: false,
    videoBackend: presenter.backendName,
    filterMode: prefs.filterMode,
    snapshotBytes: 0,
    snapshotSha: null,
    saveLoadOk: null,
    canvasSha: null,
    syncHash64: null,
    paused: false,
    menuOpen: false,
    modeSelectOpen: false,
    debugTier: 0,
    onlineOpen: false,
    speedPercent: 100,
    slotIdx: 0,
    dbgLayers: prefs.hd704 ? 0x08 : 0x00,
    copaMode: false,
    mainPc: '0x00000000',
    slavePc: '0x00000000',
    replayRecording: false,
    replayPlaying: false,
    replayFrames: 0,
    bench600: null,
    net: {
      connected: false,
      active: false,
      role: 'local',
      rttMs: 0,
      jitterMs: 0,
      delayFrames: 3,
      rollbacks: 0,
      maxDepth: 0,
      stalls: 0,
      syncOk: 0,
      desyncs: 0,
      firstDesyncFrame: 0,
      txKbps: 0,
      rxKbps: 0,
    },
  };
  window.__twc98G4Stats = stats;

  let worker = null;
  let lastWorkerSoloPaused = true;
  let lastSentSoloPa = -1;
  let lastSentSoloPb = -1;
  let lastSentSoloPc = -1;
  let needsPresent = false;
  let paused = true;
  let loadingKind = null; // null | 'switching' | 'restarting' | 'onlineSync'
  let modeSelectOpen = false;
  let selectedStartCopa = false; // Classic '98 highlighted by default
  let activeMainTab = 0; // 0 = Menú, 1 = Online, 2 = Controles
  const DEFAULT_SIGNAL_WORKER_URL = '';
  const DEV_CODE_SALT = '';
  const DEV_CODE_SHA256 = '';
  const isLocalHost = window.location.hostname === '127.0.0.1' || window.location.hostname === 'localhost';
  const rawDevParam = initHs.get('dev') || initQs.get('dev') || '';
  let devModeEnabled = (isLocalHost && rawDevParam === '1') || (() => {
    try { return sessionStorage.getItem('twc98_dev_unlocked') === '1'; } catch { return false; }
  })();
  let debugMode = false; // Always OFF by default; toggleable only after Dev gate unlock

  function applyDevModeUi() {
    if (devModeEnabled) {
      btnDebugToggle.classList.remove('hidden');
      if (optSigLocal) optSigLocal.classList.remove('hidden');
      if (turnConfigBlock) turnConfigBlock.classList.remove('hidden');
    } else {
      btnDebugToggle.classList.add('hidden');
      if (optSigLocal) optSigLocal.classList.add('hidden');
      if (turnConfigBlock) turnConfigBlock.classList.add('hidden');
      if (selSigMode && selSigMode.value === 'local') selSigMode.value = 'worker';
    }
  }

  function stripSensitiveUrlParams() {
    try {
      const u = new URL(window.location.href);
      let changed = false;
      if (u.searchParams.has('dev') && u.searchParams.get('dev') !== '1') {
        u.searchParams.delete('dev');
        changed = true;
      }
      if (u.hash) {
        const hp = new URLSearchParams(u.hash.slice(1));
        if (hp.has('dev') || hp.has('key')) {
          hp.delete('dev');
          hp.delete('key');
          const rest = hp.toString();
          u.hash = rest ? `#${rest}` : '';
          changed = true;
        }
      }
      if (changed) window.history.replaceState(null, '', u.toString());
    } catch {}
  }

  async function verifyAndUnlockDevCode(candidate) {
    const clean = String(candidate || '').trim();
    if (!clean || !window.crypto || !window.crypto.subtle) return false;
    const data = new TextEncoder().encode(`${DEV_CODE_SALT}${clean}`);
    const dig = await crypto.subtle.digest('SHA-256', data);
    const hex = Array.from(new Uint8Array(dig)).map((b) => b.toString(16).padStart(2, '0')).join('');
    if (hex !== DEV_CODE_SHA256) return false;
    devModeEnabled = true;
    try { sessionStorage.setItem('twc98_dev_unlocked', '1'); } catch {}
    applyDevModeUi();
    stripSensitiveUrlParams();
    syncUiState();
    return true;
  }

  applyDevModeUi();
  if (rawDevParam && !(isLocalHost && rawDevParam === '1')) {
    verifyAndUnlockDevCode(rawDevParam);
  }
  let speedIdx = 2; // 100%
  let slotIdx = 0;

  let copaMode = false;
  let copaSeed = 0x1998ca01;
  let gkFixEnabled = true;
  let hardMode = Boolean(prefs.hardMode);
  let localCoopMode = false;
  let onlineCoopMode = false;
  let matchRuleKey = String(prefs.matchRuleKey || 'default');
  function pushSoloCoopMode() {
    if (!worker || netSession.inMatch || netSession.handshaking) return;
    worker.postMessage({
      type: 'set_coop_mode',
      coopMode: Boolean(localCoopMode),
    });
  }
  function parseMatchRuleKey(key) {
    const k = String(key || 'default');
    if (k === 'time_90') return { ruleMode: 1, ruleValue: 90 };
    if (k === 'time_120') return { ruleMode: 1, ruleValue: 120 };
    if (k === 'time_180') return { ruleMode: 1, ruleValue: 180 };
    if (k === 'goals_3') return { ruleMode: 2, ruleValue: 3 };
    if (k === 'goals_5') return { ruleMode: 2, ruleValue: 5 };
    if (k === 'goals_7') return { ruleMode: 2, ruleValue: 7 };
    return { ruleMode: 0, ruleValue: 90 };
  }
  function pushSoloMatchRules() {
    if (!worker || netSession.inMatch || netSession.handshaking) return;
    worker.postMessage({
      type: 'set_match_rules',
      hardMode: Boolean(hardMode),
      ruleMode: 0,
      ruleValue: 90,
    });
  }
  let realScspAudio = Boolean(prefs.realScsp);
  let dbgLayers = prefs.hd704 ? 0x08 : 0x00;
  let lastTel = null;
  let lastRgba = null;
  const gpuPktQueue = [];
  const vsyncSab = (typeof SharedArrayBuffer !== 'undefined') ? new SharedArrayBuffer(8) : null;
  const vsyncF64 = vsyncSab ? new Float64Array(vsyncSab) : null;
  let anyGamepadSeen = false;
  let padPollCounter = 0;
  window.addEventListener('gamepadconnected', () => { anyGamepadSeen = true; });
  let lastW = 704;
  let lastH = 448;
  let lastSentLiveButtons = -1;
  let lastSentLiveSysC = -1;
  let lastOnlineStatusKey = 'onlineOfflineMsg';
  let customOnlineMsg = null;

  const savedSlots = [null, null, null];
  const scspRecent = [];
  const pressedKeys = new Set();
  const frameTimes = [];
  const renderTimes = [];
  const glTimes = [];
  const lagTimes = [];
  let profSkippedFrames = 0;
  let profDroppedPkts = 0;
  let profRenderFrames = 0;
  let profVisWinStart = 0;
  let profVisWinCount = 0;
  let profVisFps = 0;
  let profActiveMs = 0;
  let profLastTickMs = 0;
  let profLastFrameRecvMs = 0;
  let profMaxSimMs = 0;
  let profMaxRenMs = 0;
  let profStalls = 0;
  let bundleId = 'dev';
  let bundleShortId = 'dev';
  let loggedDesyncFrame = 0;
  let profBaseSimMs = null;
  let profBaseRenMs = null;

  const touchState = {
    up: false,
    down: false,
    left: false,
    right: false,
    dash: false,
    a: false,
    b: false,
    c: false,
    coin: false,
    start: false,
  };
  let joyPointerId = null;
  let joyOrigin = { cx: 0, cy: 0, radius: 56 };
  const abcPointers = new Map();
  const abcPulseTails = new Set();
  let lastSentSoloDash = false;
  let lastSentLiveDash = false;

  function isTouchPadEnabled() {
    const qTouch = initQs.get('touch');
    if (qTouch === '1') return true;
    if (qTouch === '0') return false;
    if (prefs.touchPad === 'on') return true;
    if (prefs.touchPad === 'off') return false;
    return detectedMobile;
  }

  function resetTouchInputState() {
    joyPointerId = null;
    for (const entry of abcPointers.values()) {
      if (entry.graceTimer) clearTimeout(entry.graceTimer);
      if (entry.holdTimer) clearTimeout(entry.holdTimer);
    }
    abcPointers.clear();
    for (const tail of abcPulseTails) {
      if (tail.timer) clearTimeout(tail.timer);
    }
    abcPulseTails.clear();
    touchState.up = false;
    touchState.down = false;
    touchState.left = false;
    touchState.right = false;
    touchState.dash = false;
    touchState.a = false;
    touchState.b = false;
    touchState.c = false;
    touchState.coin = false;
    touchState.start = false;
    if (tJoyZone) tJoyZone.classList.remove('active', 'sprinting');
    if (tJoyAnchor) {
      tJoyAnchor.style.left = '';
      tJoyAnchor.style.top = '';
    }
    if (tJoyKnob) tJoyKnob.style.transform = 'translate(-50%, -50%)';
    if (tBtnA) tBtnA.classList.remove('active', 'combo-active');
    if (tBtnB) tBtnB.classList.remove('active', 'combo-active');
    if (tBtnC) tBtnC.classList.remove('active', 'combo-active');
    if (tBtnAbc) tBtnAbc.classList.remove('active');
    if (tRouteAB) tRouteAB.classList.remove('active');
    if (tRouteAC) tRouteAC.classList.remove('active');
    if (tRouteBC) tRouteBC.classList.remove('active');
    if (tPillAB) tPillAB.classList.remove('active');
    if (tPillAC) tPillAC.classList.remove('active');
    if (tPillBC) tPillBC.classList.remove('active');
    if (tBtnCoin) tBtnCoin.classList.remove('active');
    if (tBtnStart) tBtnStart.classList.remove('active');
  }

  const replay = {
    recording: false,
    playing: false,
    initSnapshot: null,
    frames: [],
    cursor: 0,
  };

  let rtcClient = null;
  const netSession = {
    connected: false,
    handshaking: false,
    inMatch: false,
    role: 'local',
    localPlayer: 0,
    delayFrames: 3,
    recommendedDelay: 3,
    rttMs: 0,
    jitterMs: 0,
    pendingPrepResolve: null,
    netStats: null,
  };

  const pendingReqs = new Map();
  let nextReqId = 1;

  function sendWorkerReq(type, payload = {}, transfers = []) {
    return new Promise((resolve, reject) => {
      if (!worker) {
        reject(new Error('Worker not initialized'));
        return;
      }
      const reqId = nextReqId++;
      pendingReqs.set(reqId, { resolve, reject });
      worker.postMessage({ ...payload, type, reqId }, transfers);
    });
  }

  function formatKeyCode(code) {
    if (!code) return '-';
    if (code.startsWith('Key')) return code.slice(3);
    if (code.startsWith('Digit')) return code.slice(5);
    if (code.startsWith('Numpad')) return 'Num' + code.slice(6);
    if (code === 'Space') return 'Space';
    if (code === 'ArrowUp') return '↑';
    if (code === 'ArrowDown') return '↓';
    if (code === 'ArrowLeft') return '←';
    if (code === 'ArrowRight') return '→';
    if (code === 'Comma') return ',';
    if (code === 'Period') return '.';
    if (code === 'Slash') return '/';
    if (code === 'ShiftRight') return 'RShift';
    if (code === 'ShiftLeft') return 'LShift';
    if (code === 'Enter') return 'Enter';
    return code;
  }

  async function sha256Hex(u8) {
    const digest = await crypto.subtle.digest('SHA-256', u8);
    return Array.from(new Uint8Array(digest)).map((b) => b.toString(16).padStart(2, '0')).join('');
  }

  async function updateCanvasHash() {
    if (!debugMode) return null;
    if (mtActive && renderWorker) {
      const h = await new Promise((resolve) => {
        const reqId = nextReqId++;
        const onMsg = (ev) => {
          const m = ev.data || {};
          if (m.type === 'canvas_sha' && m.reqId === reqId) {
            renderWorker.removeEventListener('message', onMsg);
            resolve(m.hex || null);
          }
        };
        renderWorker.addEventListener('message', onMsg);
        renderWorker.postMessage({ type: 'get_canvas_sha', reqId });
      });
      if (h) {
        stats.canvasSha = h;
        if (elCanvasSha) elCanvasSha.textContent = h.slice(0, 20) + '...';
      }
      return h;
    }
    if (!lastRgba && gpuVdpActive && typeof presenter.readGpuVdp2Rgba === 'function') {
      const gpuPx = presenter.readGpuVdp2Rgba();
      if (!gpuPx) return null;
      const h = await sha256Hex(gpuPx);
      stats.canvasSha = h;
      if (elCanvasSha) elCanvasSha.textContent = h.slice(0, 20) + '...';
      return h;
    }
    if (!lastRgba) return null;
    const h = await sha256Hex(lastRgba);
    stats.canvasSha = h;
    if (elCanvasSha) elCanvasSha.textContent = h.slice(0, 20) + '...';
    return h;
  }

  function ensureAudio() {
    audioEngine.ensureStarted();
    audioEngine.setMuted(Boolean(prefs.muted));
    audioEngine.setVolume(prefs.volumePct);
  }

  function playWorkerAudio(msg) {
    const evs = msg.scspEvents || msg.audioEvents;
    if (evs && evs.length > 0) {
      stats.scspEvents += evs.length;
      for (const ev of evs) {
        if (ev.keyOn || ev.type === 1) stats.audioEventsTriggered++;
        scspRecent.push(`f=${msg.framesRun ?? stats.framesRun} [${ev.addr ?? ev.slot}] ${ev.keyOn ? 'KEYON' : 'OFF'} k=${ev.key ?? 0}`);
      }
      while (scspRecent.length > 6) scspRecent.shift();
      if (debugMode && elScspLog) elScspLog.textContent = scspRecent.join('\n');
    }
    const pcmBuf = (msg.pcm instanceof ArrayBuffer)
      ? msg.pcm
      : (msg.pcmBuf instanceof ArrayBuffer
        ? msg.pcmBuf
        : (msg.pcm && msg.pcm.buffer instanceof ArrayBuffer ? msg.pcm.buffer : null));
    const pcmFrames = msg.pcmFrames ?? msg.pcmSamples ?? 0;
    if (pcmBuf && pcmFrames > 0) {
      audioEngine.pushPcmBuffer(pcmBuf);
    }
  }

  function currentVideoBackendLabel() {
    if (mtActive) return presenter.backendName;
    if (gpuVdpActive) {
      const sc = (typeof presenter.getUltraScale === 'function') ? presenter.getUltraScale() : 1;
      const vTag = gpuVdp1Active ? 'VDP1+2' : 'VDP2';
      return sc >= 2 ? `${presenter.backendName}-${vTag}-Ultra2x` : `${presenter.backendName}-${vTag}-GPU`;
    }
    if (bandPoolActive) return `${presenter.backendName}-Bands(${bandWorkers.length})`;
    return presenter.backendName;
  }

  function applyVideoSettings() {
    presenter.setFilterMode(prefs.filterMode);
    presenter.setCrtMask(Boolean(prefs.crtMask));
    if (typeof presenter.setHdMode === 'function') presenter.setHdMode(Boolean(prefs.hdMode));
    presenter.setUseGpu(Boolean(prefs.useGpu));
    stats.videoBackend = currentVideoBackendLabel();
    stats.filterMode = prefs.filterMode;

    canvas.classList.remove('aspect-4-3', 'aspect-stretch');
    if (prefs.aspect === '4:3') canvas.classList.add('aspect-4-3');
    else if (prefs.aspect === 'stretch') canvas.classList.add('aspect-stretch');
    presenter.invalidateSize();
    if (!mtActive && typeof presenter.repaintLast === 'function') {
      presenter.repaintLast();
    }
  }

  function recycleGpuPktBuf(buf) {
    if (buf && worker) {
      try { worker.postMessage({ type: 'recycle_gpu_pkt', buf }, [buf]); } catch {}
    }
  }

  function clearGpuPktQueue() {
    while (gpuPktQueue.length > 0) {
      const item = gpuPktQueue.shift();
      if (item && item.buf) recycleGpuPktBuf(item.buf);
    }
  }

  function paintFrame(msg, deferPresent = false) {
    if (msg.gpuPkt && msg.gpuPkt.byteLength > 0 && msg.gpuPktLen > 0) {
      const pktLen = msg.gpuPktLen | 0;
      const pktView = new Uint8Array(msg.gpuPkt, 0, pktLen);
      const pktI32 = (pktLen >= 1024) ? new Int32Array(msg.gpuPkt, 0, pktLen >> 2) : null;
      // Always apply dirty VDP2/VDP1 VRAM chunks in strict FIFO arrival order so skipped rAF frames never lose VRAM updates
      if (typeof presenter.uploadGpuVdp2Vram === 'function') {
        presenter.uploadGpuVdp2Vram(pktView, pktI32);
      }
      pendingBandFrame = null;
      if (lastRgba && lastRgba !== bandDisplayBuf && lastRgba.buffer && !debugMode) {
        const oldRgbaBuf = lastRgba.buffer;
        lastRgba = null;
        if (worker) {
          try { worker.postMessage({ type: 'recycle_rgba', buf: oldRgbaBuf }, [oldRgbaBuf]); } catch {}
        }
      } else {
        lastRgba = null;
      }
      lastW = msg.w || lastW;
      lastH = msg.h || lastH;
      if (deferPresent) {
        gpuPktQueue.push({ buf: msg.gpuPkt, u8: pktView, i32: pktI32, len: pktLen, w: lastW, h: lastH });
        while (gpuPktQueue.length > 2) {
          const dropped = gpuPktQueue.shift();
          profDroppedPkts++;
          if (dropped && dropped.buf) recycleGpuPktBuf(dropped.buf);
        }
        needsPresent = true;
      } else {
        clearGpuPktQueue();
        needsPresent = false;
        presenter.presentGpuVdp2(pktView, pktI32);
        recycleGpuPktBuf(msg.gpuPkt);
      }
      return;
    }
    if (msg.rgba && msg.rgba.byteLength > 0 && msg.w && msg.h) {
      clearGpuPktQueue();
      pendingBandFrame = null;
      const prevBuf = (!debugMode && lastRgba && lastRgba !== bandDisplayBuf && lastRgba.buffer && lastRgba.buffer !== msg.rgba)
        ? lastRgba.buffer
        : null;
      lastRgba = new Uint8Array(msg.rgba);
      lastW = msg.w;
      lastH = msg.h;
      if (deferPresent) {
        needsPresent = true;
      } else {
        needsPresent = false;
        presenter.present(lastRgba, lastW, lastH);
      }
      if (prevBuf && worker) {
        try { worker.postMessage({ type: 'recycle_rgba', buf: prevBuf }, [prevBuf]); } catch {}
      }
    }
  }

  function applyTelemetry(tel) {
    if (!tel) return;
    lastTel = tel;
    stats.halted = Boolean(tel.halted);
    if (typeof tel.realFps === 'number' && tel.realFps > 0) {
      stats.fps = tel.realFps;
    }
    if (tel.copaReturnedToTitle && copaMode && stats.ready && !netSession.inMatch && !modeSelectOpen) {
      openModeSelect();
    }
    if (tel.partial) return;

    stats.wasmLinearMemoryBytes = tel.wasmLinearMemoryBytes || 0;
    stats.wasmMemMB = Number((stats.wasmLinearMemoryBytes / (1024 * 1024)).toFixed(2));
    if (performance.memory && performance.memory.usedJSHeapSize) {
      stats.jsHeapMB = Number((performance.memory.usedJSHeapSize / (1024 * 1024)).toFixed(2));
    }
    stats.iogaReads = tel.iogaReads || 0;
    stats.cartReads = tel.cartReads || 0;
    stats.soundramWrites = tel.soundramWrites || 0;
    stats.mainPc = tel.mainPc || '0x00000000';
    stats.slavePc = tel.slavePc || '0x00000000';
    stats.dbgLayers = tel.dbgLayers || 0;
    if (tel.syncHash64) stats.syncHash64 = tel.syncHash64;
    stats.m68kEnabled = Boolean(tel.m68kEnabled);
    copaMode = (tel.gameMode || 0) !== 0;
    stats.copaMode = copaMode;
    gkFixEnabled = true;
    dbgLayers = stats.dbgLayers & 0x0f;
  }

  function applyTranslations() {
    const lang = resolveLangCode();
    document.documentElement.lang = lang;
    document.title = t('pageTitle');

    brandSub.textContent = t('brandSub');
    if (lblDiscordCommunity) lblDiscordCommunity.textContent = t('discordBtn');
    lblLang.textContent = t('langLabel');
    const optAuto = selLang.querySelector('option[value="auto"]');
    if (optAuto) optAuto.textContent = t('langAuto');
    btnDebugToggle.textContent = t('debugBtn');

    modeSelectTitle.textContent = t('selectModeTitle');
    liClassic1.textContent = t('classicB1');
    liClassic2.textContent = t('classicB2');
    liCopa1.textContent = t('copaB1');
    liCopa2.textContent = t('copaB2');
    btnStartSelectedMode.textContent = t('startBtn');
    btnStartPlayOnline.textContent = t('playOnlineBtn');
    if (btnStartTournamentSoon) btnStartTournamentSoon.textContent = t('tournamentSoonBtn');
    pauseBannerText.textContent = t('pausedOverlay');

    btnFullscreenFooter.textContent = t('fullscreenBtn');
    mainTabButtons[0].textContent = t('tabMenu');
    mainTabButtons[1].textContent = t('tabOnline');
    mainTabButtons[2].textContent = t('tabControls');

    sumGame.textContent = t('sumGame');
    sumVideo.textContent = t('sumVideo');
    sumAudio.textContent = t('sumAudio');
    lblMenuMode.textContent = t('gameModeLbl');
    hintMenuHostLock.textContent = t('hostLockHint');
    if (lblMenuHardMode) lblMenuHardMode.textContent = t('hardModeLbl');
    if (hintMenuHardMode) hintMenuHardMode.textContent = t('hardModeHint');
    if (lblMenuLocalCoop) lblMenuLocalCoop.textContent = t('localCoopLbl');
    if (hintMenuLocalCoop) hintMenuLocalCoop.textContent = t('localCoopHint');
    if (lblMenuTouchPad) lblMenuTouchPad.textContent = t('touchPadLbl');
    if (hintMenuTouchPad) hintMenuTouchPad.textContent = t('touchPadHint');

    if (lblVidHdMode) lblVidHdMode.textContent = t('vidHdModeLbl');
    if (hintVidHdMode) hintVidHdMode.textContent = t('vidHdModeHint');
    lblVidFilter.textContent = t('vidFilterLbl');
    lblVidCrt.textContent = t('crtMaskLbl');
    hintVidCrt.textContent = t('crtMaskHint');
    lblVidHd.textContent = t('hd704Lbl');
    hintVidHd.textContent = t('hd704Hint');
    if (lblVidGpuVdp2) lblVidGpuVdp2.textContent = t('gpuVdp2Lbl');
    if (hintVidGpuVdp2) hintVidGpuVdp2.textContent = t('gpuVdp2Hint');
    if (lblVidLowRes224) lblVidLowRes224.textContent = t('lowRes224Lbl');
    if (hintVidLowRes224) hintVidLowRes224.textContent = t('lowRes224Hint');
    if (lblVidFpsMode) lblVidFpsMode.textContent = t('fpsModeLbl');
    if (hintVidFpsMode) hintVidFpsMode.textContent = t('fpsModeHint');
    if (btnVidFpsAuto) btnVidFpsAuto.textContent = t('fpsAutoLbl');
    if (lowPerfBannerText) lowPerfBannerText.textContent = t('lowPerfWarnMsg');
    if (btnLowPerfAudioLite) btnLowPerfAudioLite.textContent = realScspAudio ? t('audioLiteBtn') : t('audioLiteOnBtn');
    lblVidAspect.textContent = t('aspectLbl');
    btnVidFullscreen.textContent = t('fullscreenBtn');
    lblAudMute.textContent = t('audSoundLbl');
    lblAudVol.textContent = t('audVolLbl');

    lblOnlineTitle.textContent = t('onlineTitle');
    lblOnlineMode.textContent = t('onlineModeLbl');
    if (lblOnlineFormat) lblOnlineFormat.textContent = t('onlineFormatLbl');
    if (btnOnlineFmtVs) btnOnlineFmtVs.textContent = t('onlineFmtVs');
    if (btnOnlineFmtCoop) btnOnlineFmtCoop.textContent = t('onlineFmtCoop');
    if (hintOnlineFormat) hintOnlineFormat.textContent = t('onlineFormatHint');
    if (lblOnlineRules) lblOnlineRules.textContent = t('onlineRulesLbl');
    if (lblOnlineHardMode) lblOnlineHardMode.textContent = t('onlineHardLbl');
    if (hintOnlineRulesLock) hintOnlineRulesLock.textContent = t('onlineRulesLockHint');
    lblRoomCode.textContent = t('roomCodeLbl');
    btnHostRoom.textContent = t('hostRoomBtn');
    btnJoinRoom.textContent = t('joinRoomBtn');
    btnStartOnlineMatch.textContent = t('startMatchBtn');
    btnOnlineRestart.textContent = t('restartHostBtn');
    btnLeaveOnline.textContent = t('leaveOnlineBtn');
    sumOnlineAdv.textContent = t('sumOnlineAdv');
    lblSigMode.textContent = t('sigModeLbl');
    const oLoc = selSigMode.querySelector('option[value="local"]');
    const oWrk = selSigMode.querySelector('option[value="worker"]');
    const oMan = selSigMode.querySelector('option[value="manual"]');
    if (oLoc) oLoc.textContent = t('sigLocal');
    if (oWrk) oWrk.textContent = t('sigWorker');
    if (oMan) oMan.textContent = t('sigManual');
    lblNetDelay.textContent = t('delayLbl');
    lblWorkerUrl.textContent = t('workerUrlLbl');
    btnApplyManualToken.textContent = t('applyTokenBtn');

    hintControlsTop.textContent = t('ctrlTopHint');
    lblCtrlP1Title.textContent = t('ctrlP1Title');
    lblCtrlP2Title.textContent = t('ctrlP2Title');
    btnResetP1.textContent = t('resetDefaultBtn');
    btnResetP2.textContent = t('resetDefaultBtn');
    if (lblPadP1Title) lblPadP1Title.textContent = t('padP1Title');
    if (lblPadP2Title) lblPadP2Title.textContent = t('padP2Title');
    if (btnResetPad1) btnResetPad1.textContent = t('resetDefaultBtn');
    if (btnResetPad2) btnResetPad2.textContent = t('resetDefaultBtn');
    if (btnPadSwap) btnPadSwap.textContent = t('padSwapBtn');

    accessTitle.textContent = t('vaultTitle');
    lblAccessPasscode.textContent = t('vaultPassLbl');
    btnUnlockVault.textContent = t('vaultUnlock');
    btnClearVaultCache.textContent = t('vaultClear');
    controlsDocBar.innerHTML = t('debugHotkeysNotice');

    if (!customOnlineMsg) {
      onlineStatusBox.textContent = t(lastOnlineStatusKey);
    }
    renderRemapGrids();
    renderPadRemapGrids();
  }

  function renderOneRemapGrid(container, actionKeys, mapObj, labelsMap, playerNum) {
    if (!container) return;
    container.innerHTML = '';
    for (const action of actionKeys) {
      const row = document.createElement('div');
      row.className = 'remap-item';
      const lbl = document.createElement('span');
      lbl.textContent = labelsMap[action] || action;
      const btn = document.createElement('button');
      btn.type = 'button';
      const isListening = listeningTarget && listeningTarget.player === playerNum && listeningTarget.action === action;
      btn.className = 'remap-btn' + (isListening ? ' listening' : '');
      btn.textContent = isListening ? t('pressKeyPrompt') : formatKeyCode(mapObj[action]);
      btn.onclick = () => {
        listeningPadTarget = null;
        listeningTarget = isListening ? null : { player: playerNum, action };
        renderRemapGrids();
        renderPadRemapGrids();
      };
      row.appendChild(lbl);
      row.appendChild(btn);
      container.appendChild(row);
    }
  }

  function renderRemapGrids() {
    renderOneRemapGrid(controlsRemapP1, P1_ACTION_KEYS, p1KeyMap, t('actionsP1'), 1);
    renderOneRemapGrid(controlsRemapP2, P2_ACTION_KEYS, p2KeyMap, t('actionsP2'), 2);
  }

  function getConnectedPads() {
    if (!navigator.getGamepads) return [];
    const list = [];
    const raw = navigator.getGamepads();
    for (let i = 0; i < raw.length; i++) {
      const gp = raw[i];
      if (gp && gp.connected) list.push(gp);
    }
    return list;
  }

  function getAssignedPadPair() {
    const pads = getConnectedPads();
    if (pads.length === 0) return { pads, pad1: null, pad2: null };
    if (pads.length === 1) {
      return { pads, pad1: pads[0], pad2: null };
    }
    const swap = Boolean(padPrefs.swapPads);
    return {
      pads,
      pad1: swap ? pads[1] : pads[0],
      pad2: swap ? pads[0] : pads[1],
    };
  }

  function formatPadBind(bind) {
    if (!bind) return '-';
    if (bind.type === 'axis') {
      const sign = bind.dir < 0 ? '-' : '+';
      return `Ax${bind.index}${sign}`;
    }
    const idx = bind.index | 0;
    const names = { 0: 'B0 (A/✕)', 1: 'B1 (B/○)', 2: 'B2 (C/□)', 3: 'B3 (D/△)', 4: 'L1', 5: 'R1', 6: 'L2', 7: 'R2', 8: 'Select', 9: 'Start', 12: 'D-Up', 13: 'D-Down', 14: 'D-Left', 15: 'D-Right' };
    return names[idx] || `B${idx}`;
  }

  function captureGamepadBaseline(gp) {
    if (!gp) return null;
    return {
      btns: Array.from(gp.buttons || [], (b) => Boolean(b && b.pressed)),
      axes: Array.from(gp.axes || [], (a) => Number(a || 0)),
    };
  }

  function renderOnePadRemapGrid(container, actionKeys, mapObj, labelsMap, playerNum, padObj) {
    if (!container) return;
    container.innerHTML = '';
    for (const action of actionKeys) {
      const row = document.createElement('div');
      row.className = 'remap-item';
      const lbl = document.createElement('span');
      lbl.textContent = labelsMap[action] || action;
      const btn = document.createElement('button');
      btn.type = 'button';
      const isListening = listeningPadTarget && listeningPadTarget.player === playerNum && listeningPadTarget.action === action;
      btn.className = 'remap-btn' + (isListening ? ' listening' : '');
      btn.textContent = isListening ? t('pressPadPrompt') : formatPadBind(mapObj[action]);
      btn.disabled = !padObj;
      btn.onclick = () => {
        listeningTarget = null;
        if (isListening || !padObj) {
          listeningPadTarget = null;
          padListenBaseline = null;
        } else {
          listeningPadTarget = { player: playerNum, action, padIndex: padObj.index };
          padListenBaseline = captureGamepadBaseline(padObj);
        }
        renderRemapGrids();
        renderPadRemapGrids();
      };
      row.appendChild(lbl);
      row.appendChild(btn);
      container.appendChild(row);
    }
  }

  function renderPadRemapGrids() {
    const { pad1, pad2 } = getAssignedPadPair();
    renderOnePadRemapGrid(controlsRemapPad1, P1_ACTION_KEYS, p1PadMap, t('actionsP1'), 1, pad1);
    renderOnePadRemapGrid(controlsRemapPad2, P2_ACTION_KEYS, p2PadMap, t('actionsP2'), 2, pad2);
  }

  function pollGamepadBindCapture() {
    if (!listeningPadTarget) return;
    const pads = getConnectedPads();
    const gp = pads.find((p) => p.index === listeningPadTarget.padIndex) || pads[0];
    if (!gp) {
      listeningPadTarget = null;
      padListenBaseline = null;
      renderPadRemapGrids();
      return;
    }
    const base = padListenBaseline || captureGamepadBaseline(gp);
    for (let i = 0; i < gp.buttons.length; i++) {
      const nowDown = Boolean(gp.buttons[i] && gp.buttons[i].pressed);
      const wasDown = Boolean(base.btns[i]);
      if (nowDown && !wasDown) {
        const prev = (listeningPadTarget.player === 1 ? p1PadMap : p2PadMap)[listeningPadTarget.action];
        const nextBind = { type: 'btn', index: i };
        if (prev && typeof prev.axis === 'number' && (i >= 12 && i <= 15)) {
          nextBind.axis = prev.axis;
          nextBind.dir = prev.dir;
        }
        if (listeningPadTarget.player === 1) {
          p1PadMap[listeningPadTarget.action] = nextBind;
          saveJsonPrefs(PADMAP_P1_KEY, p1PadMap);
        } else {
          p2PadMap[listeningPadTarget.action] = nextBind;
          saveJsonPrefs(PADMAP_P2_KEY, p2PadMap);
        }
        listeningPadTarget = null;
        padListenBaseline = null;
        renderPadRemapGrids();
        syncUiState();
        return;
      }
      if (!nowDown && wasDown) base.btns[i] = false;
    }
    for (let a = 0; a < gp.axes.length; a++) {
      const v = Number(gp.axes[a] || 0);
      const bv = Number(base.axes[a] || 0);
      if (Math.abs(v) > 0.68 && Math.abs(bv) < 0.45) {
        const nextBind = { type: 'axis', index: a, dir: v > 0 ? 1 : -1 };
        if (listeningPadTarget.player === 1) {
          p1PadMap[listeningPadTarget.action] = nextBind;
          saveJsonPrefs(PADMAP_P1_KEY, p1PadMap);
        } else {
          p2PadMap[listeningPadTarget.action] = nextBind;
          saveJsonPrefs(PADMAP_P2_KEY, p2PadMap);
        }
        listeningPadTarget = null;
        padListenBaseline = null;
        renderPadRemapGrids();
        syncUiState();
        return;
      }
      if (Math.abs(v) < 0.3) base.axes[a] = v;
    }
  }

  function setOnlineLogKey(key) {
    customOnlineMsg = null;
    lastOnlineStatusKey = key;
    onlineStatusBox.textContent = t(key);
  }

  function setOnlineLogText(text) {
    customOnlineMsg = text;
    onlineStatusBox.textContent = text;
  }

  const statsSortScratch = new Float64Array(128);
  function calcStatsP50P95(arr, outObj) {
    const n = Math.min(arr.length, statsSortScratch.length);
    if (n <= 0) {
      outObj.avg = 0; outObj.p50 = 0; outObj.p95 = 0;
      return outObj;
    }
    let sum = 0;
    for (let i = 0; i < n; i++) {
      const v = arr[i];
      statsSortScratch[i] = v;
      sum += v;
    }
    const view = statsSortScratch.subarray(0, n);
    view.sort();
    outObj.avg = sum / n;
    outObj.p50 = view[Math.floor(n * 0.5)];
    outObj.p95 = view[Math.floor(n * 0.95)];
    return outObj;
  }
  const tmpStatsA = { avg: 0, p50: 0, p95: 0 };
  const tmpStatsB = { avg: 0, p50: 0, p95: 0 };

  function updateHudTelemetry() {
    if (elFrames) elFrames.textContent = String(stats.framesRun);
    if (elPcs) elPcs.textContent = `${stats.mainPc} / ${stats.slavePc}`;
    if (elMem) elMem.textContent = `${stats.wasmMemMB} MiB`;
    if (elScsp) elScsp.textContent = `${stats.scspEvents} (${stats.m68kEnabled ? 'M68K ON' : 'SCSP'} | ${stats.soundramWrites} wr)`;
    if (elCart) elCart.textContent = String(stats.cartReads);
    if (elSyncHash) elSyncHash.textContent = stats.syncHash64 || '-';

    if (frameTimes.length > 0) {
      const simSt = calcStatsP50P95(frameTimes, tmpStatsA);
      stats.avgMs = Number(simSt.avg.toFixed(2));
      stats.p50Ms = Number(simSt.p50.toFixed(2));
      stats.p95Ms = Number(simSt.p95.toFixed(2));
      const shownFps = (lastTel && typeof lastTel.realFps === 'number' && lastTel.realFps > 0)
        ? lastTel.realFps
        : (stats.avgMs > 0 ? Number(Math.min(60, 1000 / stats.avgMs).toFixed(1)) : 0);
      stats.fps = shownFps;
      const renSt = calcStatsP50P95(renderTimes, tmpStatsB);
      const renAvg = Number(renSt.avg.toFixed(2));
      if (elFps) elFps.textContent = `${shownFps} fps (sim ${stats.avgMs} + ren ${renAvg} ms, ${stats.speedPercent}%)`;
      if (profEnabled && (profBar || fsProfBar)) {
        const rP50Num = renSt.p50;
        const rP50 = rP50Num.toFixed(2);
        const rP95 = renSt.p95.toFixed(2);
        const glSt = calcStatsP50P95(glTimes, tmpStatsB);
        const gP50 = glSt.p50.toFixed(2);
        const gP95 = glSt.p95.toFixed(2);
        const lagSt = calcStatsP50P95(lagTimes, tmpStatsB);
        const lP95 = lagSt.p95.toFixed(1);
        const vFps = mtActive ? renderWorkerVisFps : profVisFps;
        const und = audioEngine.getUnderrunCount();
        const bufF = audioEngine.getBufferedFrames();
        const totalP50 = (stats.p50Ms || 0) + rP50Num;
        const cpuLoadPct = Math.round((totalP50 / 16.67) * 100);
        let thermTxt = `cpu ${cpuLoadPct}%`;
        if (profBaseSimMs !== null) {
          const baseTotal = Math.max(0.25, profBaseSimMs + (profBaseRenMs || 0));
          const driftPct = Math.round(((totalP50 - baseTotal) / baseTotal) * 100);
          thermTxt += ` (Δ${driftPct >= 0 ? '+' : ''}${driftPct}%)`;
        } else {
          thermTxt += ' (base...)';
        }
        const totSec = Math.floor(profActiveMs / 1000);
        const mm = String(Math.floor(totSec / 60)).padStart(2, '0');
        const ss = String(totSec % 60).padStart(2, '0');
        const hdTag = prefs.hdMode ? `${(typeof presenter.getUltraScale === 'function') ? presenter.getUltraScale() : 1}x` : '0';
        const glTag = (presenter && typeof presenter.getGpuDiagTag === 'function') ? presenter.getGpuDiagTag() : 'n/a';
        const resTag = prefs.lowRes224p ? '224p' : '448p';
        const modeTag = `fcpu=${fastCpuEnabled ? 1 : 0} aot=${sh2AotEnabled ? 1 : 0} v1gpu=${gpuVdp1Active ? 1 : 0} gpu=${gpuVdpActive ? 1 : 0} gl=${glTag} ${resTag} re=${pacerState.renderEvery}(${pacerState.mode}) hd=${hdTag} bands=${bandPoolActive ? bandWorkers.length : 0} mt=${mtActive ? 1 : 0} idle=${idleSkipEnabled ? 1 : 0} sab=${audioEngine.getSharedAudioSab() ? 1 : 0} audSt=${audioEngine.getStateTag()} v=${bundleShortId}`;
        if (profBar) {
          profBar.textContent = `sim ${stats.p50Ms}/${stats.p95Ms} ms | gl ${gP50}/${gP95} ms | ren ${rP50}/${rP95} ms | max ${profMaxSimMs.toFixed(1)}/${profMaxRenMs.toFixed(1)} st:${profStalls} | fps ${shownFps}/${vFps || '--'} | ${thermTxt} | t ${mm}:${ss} | skip ${profSkippedFrames} drop ${profDroppedPkts} | lag95 ${lP95} ms | aud ${bufF}f und:${und} | ${modeTag}`;
        }
        if (fsProfBar) {
          fsProfBar.textContent = `FPS ${vFps || '--'}/${shownFps} | sim ${stats.p50Ms}ms | gl ${gP50}ms | ${thermTxt} | drop ${profDroppedPkts} | ${resTag} ${glTag} v=${bundleShortId}`;
        }
      }
    } else if (elFps) {
      elFps.textContent = `speed ${stats.speedPercent}%`;
    }

    const ns = netSession.netStats;
    stats.net.connected = netSession.connected;
    stats.net.active = netSession.inMatch;
    stats.net.role = netSession.role;
    stats.net.rttMs = netSession.rttMs;
    stats.net.jitterMs = netSession.jitterMs;
    stats.net.delayFrames = netSession.delayFrames;
    if (rtcClient) {
      stats.net.txKbps = rtcClient.txKbps;
      stats.net.rxKbps = rtcClient.rxKbps;
    }
    if (ns) {
      stats.net.rollbacks = ns.rollbacks;
      stats.net.maxDepth = ns.maxDepth;
      stats.net.stalls = ns.stalls;
      stats.net.syncOk = ns.syncOk;
      stats.net.desyncs = ns.desyncs;
      stats.net.firstDesyncFrame = ns.firstDesyncFrame || 0;
    }

    if (netSession.inMatch && ns) {
      const desyncTag = (ns.desyncs > 0 && ns.firstDesyncFrame > 0) ? `${ns.desyncs}@f${ns.firstDesyncFrame}` : `${ns.desyncs}`;
      const fullDiag = `P${netSession.localPlayer + 1} ${netSession.rttMs}ms | d:${netSession.delayFrames}f | rb:${ns.rollbacks}(${ns.maxDepth}f) | desync:${desyncTag}`;
      if (elNetHud) elNetHud.textContent = `${fullDiag} | ok:${ns.syncOk} | tx/rx:${stats.net.txKbps}/${stats.net.rxKbps} Kbps`;
      netPill.textContent = debugMode
        ? fullDiag
        : (ns.desyncs > 0
          ? `P${netSession.localPlayer + 1} · ${netSession.rttMs} ms · DESYNC${ns.firstDesyncFrame > 0 ? ` f=${ns.firstDesyncFrame}` : ''}`
          : `P${netSession.localPlayer + 1} · ${netSession.rttMs} ms`);
      netPill.classList.toggle('warn', ns.desyncs > 0);
      netPill.classList.remove('hidden');
    } else if (netSession.connected) {
      if (elNetHud) elNetHud.textContent = `Lobby ${netSession.role.toUpperCase()} (RTT ${netSession.rttMs} ms)`;
      netPill.textContent = `Lobby P${netSession.role === 'host' ? 1 : 2} · ${netSession.rttMs} ms`;
      netPill.classList.remove('warn');
      netPill.classList.remove('hidden');
    } else {
      if (elNetHud) elNetHud.textContent = 'offline';
      netPill.classList.remove('warn');
      netPill.classList.add('hidden');
    }

    syncUiState();
  }

  function syncWorkerSoloRunState() {
    if (!worker) return;
    const wantPaused = !stats.ready || stats.halted || paused || modeSelectOpen || netSession.inMatch || netSession.handshaking;
    if (wantPaused !== lastWorkerSoloPaused) {
      lastWorkerSoloPaused = wantPaused;
      if (wantPaused) {
        audioEngine.clear();
        profLastTickMs = 0;
      } else {
        renderWorkerVisFps = 0;
        profVisFps = 0;
        profVisWinStart = 0;
        profVisWinCount = 0;
        pushLiveSoloInputToWorker(true);
      }
      worker.postMessage({ type: 'solo_set_paused', paused: wantPaused });
    }
  }

  function syncUiState() {
    const effPaused = paused || modeSelectOpen;
    stats.paused = effPaused;
    stats.menuOpen = false;
    stats.modeSelectOpen = modeSelectOpen;
    stats.debugTier = debugMode ? 2 : 0;
    stats.onlineOpen = activeMainTab === 1;
    stats.speedPercent = SPEED_PERCENTS[speedIdx];
    stats.slotIdx = slotIdx;

    const isOnlineSession = netSession.connected || netSession.inMatch || netSession.handshaking;
    const isGuestLocked = isOnlineSession && netSession.role === 'guest';

    // Top bar
    btnDebugToggle.classList.toggle('active', debugMode);
    editionPill.textContent = copaMode ? 'COPA AMÉRICA' : "CLASSIC '98";
    editionPill.classList.toggle('copa', copaMode);

    // Startup overlay card selection
    modeSelectOverlay.classList.toggle('hidden', !modeSelectOpen);
    cardClassicMode.classList.toggle('selected', !selectedStartCopa);
    cardCopaMode.classList.toggle('selected', selectedStartCopa);

    // Loading / Pause banner
    const isLoadingNow = Boolean(loadingKind || netSession.handshaking);
    const showCenterBanner = stats.ready && !modeSelectOpen && (isLoadingNow || (!netSession.inMatch && paused));
    pauseBannerText.textContent = isLoadingNow ? t('loadingOverlay') : t('pausedOverlay');
    pauseBanner.classList.toggle('hidden', !showCenterBanner);

    // Clean Footer Status + FPS
    const modeName = copaMode ? 'Copa América' : "Classic '98";
    statusDot.classList.remove('live', 'paused', 'warn');
    if (!stats.ready) {
      stageStatusText.textContent = t('statusChoosing');
    } else if (stats.halted && lastTel) {
      statusDot.classList.add('warn');
      stageStatusText.textContent = `CPU Halt (${lastTel.haltCpu})`;
    } else if (isLoadingNow) {
      statusDot.classList.add('paused');
      stageStatusText.textContent = (loadingKind === 'restarting') ? t('statusRestarting') : t('statusSwitching');
    } else if (netSession.inMatch) {
      statusDot.classList.add(stats.net.desyncs > 0 ? 'warn' : 'live');
      stageStatusText.textContent = `${t('statusOnlineMatch')} · P${netSession.localPlayer + 1} · ${modeName}`;
    } else if (netSession.connected) {
      statusDot.classList.add('live');
      stageStatusText.textContent = `${t('statusOnlineLobby')} · ${modeName}`;
    } else if (modeSelectOpen) {
      statusDot.classList.add('paused');
      stageStatusText.textContent = t('statusChoosing');
    } else if (paused) {
      statusDot.classList.add('paused');
      stageStatusText.textContent = `${t('statusPaused')} · ${modeName}`;
    } else {
      statusDot.classList.add('live');
      stageStatusText.textContent = `${t('statusReady')} · ${modeName}`;
    }

    if (stageVersionTag) {
      stageVersionTag.textContent = bundleShortId ? `v${bundleShortId}` : '';
    }
    const simFpsRaw = stats.fps || 0;
    const visFpsRaw = mtActive ? renderWorkerVisFps : profVisFps;
    if (((stats.ready && !modeSelectOpen && !paused) || netSession.inMatch) && simFpsRaw > 0) {
      const simInt = Math.round(simFpsRaw);
      const visInt = visFpsRaw > 0 ? Math.round(visFpsRaw) : simInt;
      const effFps = Math.min(simInt, visInt);
      const visGap = visFpsRaw > 0 && visInt < simInt - 3;
      stageFpsClean.textContent = visGap ? `${effFps} FPS (sim ${simInt})` : `${effFps} FPS`;
      stageFpsClean.classList.toggle('warn', visGap || effFps < 50);
    } else {
      stageFpsClean.textContent = '-- FPS';
      stageFpsClean.classList.remove('warn');
    }

    const showTouch = Boolean(stats.ready && !modeSelectOpen && !isLoadingNow && isTouchPadEnabled() && (netSession.inMatch || !paused));
    if (touchControlsOverlay) {
      const wasHidden = touchControlsOverlay.classList.contains('hidden');
      touchControlsOverlay.classList.toggle('hidden', !showTouch);
      if (!showTouch && !wasHidden) resetTouchInputState();
    }

    btnCoin1Footer.textContent = `Coin ${netSession.inMatch && netSession.localPlayer === 1 ? 2 : 1} (${formatKeyCode(p1KeyMap.coin1)})`;
    btnStart1Footer.textContent = `${netSession.inMatch && netSession.localPlayer === 1 ? '2P' : '1P'} Start (${formatKeyCode(p1KeyMap.start1)})`;
    btnRestartFooter.textContent = isOnlineSession ? t('restartHostBtn') : t('restartBtn');
    btnRestartFooter.disabled = !stats.ready || netSession.handshaking || isGuestLocked;

    const p2Card = controlsRemapP2 ? controlsRemapP2.parentElement : null;
    if (p2Card) {
      p2Card.style.opacity = isOnlineSession ? '0.45' : '';
      p2Card.style.pointerEvents = isOnlineSession ? 'none' : '';
    }

    // Main Tabs
    mainTabButtons.forEach((b, idx) => b.classList.toggle('active', idx === activeMainTab));
    mainTabPanes.forEach((p, idx) => p.classList.toggle('hidden', idx !== activeMainTab));

    // Menú -> Game controls (Host-locked in Online)
    const canPauseSolo = stats.ready && !modeSelectOpen && !isOnlineSession;
    btnMenuPause.textContent = paused ? t('resumeBtn') : t('pauseBtn');
    btnMenuPause.disabled = !canPauseSolo;
    if (btnScreenPause) {
      btnScreenPause.classList.toggle('hidden', !canPauseSolo);
      btnScreenPause.classList.toggle('paused', paused);
      btnScreenPause.textContent = paused ? `▶ ${t('resumeBtn')}` : `⏸ ${t('pauseBtn')}`;
    }
    btnMenuRestart.textContent = isOnlineSession ? t('restartHostBtn') : t('restartBtn');
    btnMenuRestart.disabled = !stats.ready || netSession.handshaking || isGuestLocked;

    btnMenuEdClassic.classList.toggle('active', !copaMode);
    btnMenuEdCopa.classList.toggle('active', copaMode);
    btnMenuEdClassic.disabled = !stats.ready || netSession.handshaking || isGuestLocked;
    btnMenuEdCopa.disabled = !stats.ready || netSession.handshaking || isGuestLocked;
    hintMenuHostLock.classList.toggle('hidden', !isOnlineSession);

    if (btnMenuHardMode) {
      btnMenuHardMode.textContent = hardMode ? 'ON' : 'OFF';
      btnMenuHardMode.classList.toggle('active', hardMode);
      btnMenuHardMode.disabled = !stats.ready || isOnlineSession;
    }
    if (btnMenuLocalCoop) {
      btnMenuLocalCoop.textContent = localCoopMode ? 'ON' : 'OFF';
      btnMenuLocalCoop.classList.toggle('active', localCoopMode);
      btnMenuLocalCoop.disabled = !stats.ready || isOnlineSession;
    }

    if (btnMenuTouchPad) {
      const tpMode = (prefs.touchPad === 'on' || prefs.touchPad === 'off') ? prefs.touchPad : 'auto';
      btnMenuTouchPad.textContent = tpMode === 'on' ? t('touchOn') : (tpMode === 'off' ? t('touchOff') : t('touchAuto'));
      btnMenuTouchPad.classList.toggle('active', isTouchPadEnabled());
    }

    // Video controls
    if (btnVidHdMode) {
      btnVidHdMode.textContent = prefs.hdMode ? 'ON' : 'OFF';
      btnVidHdMode.classList.toggle('active', Boolean(prefs.hdMode));
    }
    filterButtons.forEach((b, idx) => {
      if (!b) return;
      b.classList.toggle('active', idx === prefs.filterMode);
      if (idx === 4) b.disabled = Boolean(detectedSoftwareGpu);
    });
    btnVidCrtMask.textContent = prefs.crtMask ? 'ON' : 'OFF';
    btnVidCrtMask.classList.toggle('active', prefs.crtMask);
    btnVidHd704.textContent = (dbgLayers & 0x08) ? 'ON' : 'OFF';
    btnVidHd704.classList.toggle('active', (dbgLayers & 0x08) !== 0);
    if (btnVidGpuVdp2) {
      btnVidGpuVdp2.disabled = !gpuVdpSupported;
      btnVidGpuVdp2.textContent = gpuVdpSupported ? (gpuVdpActive ? 'ON' : 'OFF') : 'N/A';
      btnVidGpuVdp2.classList.toggle('active', Boolean(gpuVdpActive));
    }
    if (btnVidLowRes224) {
      btnVidLowRes224.textContent = prefs.lowRes224p ? 'ON' : 'OFF';
      btnVidLowRes224.classList.toggle('active', Boolean(prefs.lowRes224p));
    }
    const curFpsMode = prefs.fpsMode || 'auto';
    if (btnVidFpsAuto) btnVidFpsAuto.classList.toggle('active', curFpsMode === 'auto');
    if (btnVidFps60) btnVidFps60.classList.toggle('active', curFpsMode === '60');
    if (btnVidFps30) btnVidFps30.classList.toggle('active', curFpsMode === '30');
    if (lowPerfBanner) {
      const showWarn = Boolean(pacerState.below30Warning && !lowPerfDismissed && !paused && !modeSelectOpen);
      lowPerfBanner.classList.toggle('hidden', !showWarn);
      if (btnLowPerfAudioLite) {
        btnLowPerfAudioLite.textContent = realScspAudio ? t('audioLiteBtn') : t('audioLiteOnBtn');
      }
    }
    btnVidAspectFit.classList.toggle('active', prefs.aspect === 'fit');
    btnVidAspect43.classList.toggle('active', prefs.aspect === '4:3');
    btnVidAspectStretch.classList.toggle('active', prefs.aspect === 'stretch');

    // Audio controls
    const mutedNow = Boolean(prefs.muted);
    btnAudMute.textContent = mutedNow ? 'OFF' : 'ON';
    btnAudMute.classList.toggle('active', !mutedNow);
    rngVolume.value = String(prefs.volumePct);
    lblVolumePct.textContent = `${prefs.volumePct}%`;

    // Online Tab
    const sigMode = selSigMode.value;
    workerUrlWrap.style.display = (devModeEnabled && sigMode === 'worker') ? 'block' : 'none';
    manualSdpBox.classList.toggle('hidden', sigMode !== 'manual');

    onlineRoleTag.textContent = netSession.inMatch
      ? (onlineCoopMode ? `COOP ${netSession.localPlayer + 1}P` : `LIVE P${netSession.localPlayer + 1}`)
      : (netSession.connected ? (netSession.role === 'host' ? 'HOST P1' : 'GUEST P2') : 'OFFLINE');

    btnOnlineEdClassic.classList.toggle('active', !copaMode);
    btnOnlineEdCopa.classList.toggle('active', copaMode);
    btnOnlineEdClassic.disabled = !stats.ready || netSession.handshaking || isGuestLocked;
    btnOnlineEdCopa.disabled = !stats.ready || netSession.handshaking || isGuestLocked;

    const rulesLocked = !stats.ready || netSession.handshaking || netSession.inMatch || isGuestLocked;
    if (btnOnlineFmtVs && btnOnlineFmtCoop) {
      btnOnlineFmtVs.classList.toggle('active', !onlineCoopMode);
      btnOnlineFmtCoop.classList.toggle('active', onlineCoopMode);
      btnOnlineFmtVs.disabled = rulesLocked;
      btnOnlineFmtCoop.disabled = rulesLocked;
    }
    if (selOnlineRule) {
      if (selOnlineRule.value !== matchRuleKey) selOnlineRule.value = matchRuleKey;
      selOnlineRule.disabled = rulesLocked;
    }
    if (btnOnlineHardMode) {
      btnOnlineHardMode.textContent = hardMode ? 'ON' : 'OFF';
      btnOnlineHardMode.classList.toggle('active', hardMode);
      btnOnlineHardMode.disabled = rulesLocked;
    }

    btnStartOnlineMatch.disabled = !(netSession.connected && netSession.role === 'host' && !netSession.inMatch && !netSession.handshaking);
    btnOnlineRestart.disabled = !(netSession.connected && netSession.role === 'host' && !netSession.handshaking && netSession.inMatch);
    btnLeaveOnline.disabled = !(rtcClient !== null || netSession.connected || netSession.inMatch);
    btnHostRoom.disabled = Boolean(rtcClient !== null && netSession.role !== 'local');
    btnJoinRoom.disabled = Boolean(rtcClient !== null && netSession.role !== 'local');

    // Gamepad summary & detected-only customizer
    const { pads, pad1, pad2 } = getAssignedPadPair();
    if (menuGamepadInfo) {
      if (pads.length > 0) {
        const names = pads.map((p, idx) => `#${idx + 1} ${p.id.slice(0, 22)}`).join(' · ');
        menuGamepadInfo.textContent = `${t('gamepadConnected')} ${names}`;
      } else {
        menuGamepadInfo.textContent = t('gamepadNone');
      }
    }
    if (gamepadConfigWrap) {
      gamepadConfigWrap.classList.toggle('hidden', pads.length === 0);
      if (pads.length > 0) {
        const twoOn = Boolean(padPrefs.twoPads) && !isOnlineSession;
        if (btnPadTwoMode) {
          btnPadTwoMode.textContent = padPrefs.twoPads ? t('padTwoOn') : t('padTwoOff');
          btnPadTwoMode.classList.toggle('active', Boolean(padPrefs.twoPads));
          btnPadTwoMode.disabled = isOnlineSession;
        }
        if (btnPadSwap) {
          btnPadSwap.disabled = pads.length < 2;
          btnPadSwap.classList.toggle('active', Boolean(padPrefs.swapPads) && pads.length >= 2);
        }
        if (lblPadP1Title && pad1) {
          lblPadP1Title.textContent = `${t('padP1Title')} · #${pad1.index + 1}`;
        }
        if (padCardP2) {
          padCardP2.classList.toggle('hidden', !twoOn);
          if (twoOn) {
            padCardP2.style.opacity = pad2 ? '' : '0.55';
            if (lblPadP2Title) {
              lblPadP2Title.textContent = pad2
                ? `${t('padP2Title')} · #${pad2.index + 1}`
                : `${t('padP2Title')} (conecta 2º mando)`;
            }
          }
        }
      }
    }

    // Temporary Debug Drawer (Strictly hidden unless debugMode === true)
    debugPanel.classList.toggle('hidden', !debugMode);
    controlsDocBar.classList.toggle('hidden', !debugMode);
    if (debugMode) {
      speedButtons.forEach((b, idx) => b.classList.toggle('active', idx === speedIdx));
      slotButtons.forEach((b, idx) => b.classList.toggle('active', idx === slotIdx));
      btnRec.classList.toggle('recording', replay.recording);
      btnRec.textContent = replay.recording ? 'Stop Rec (F9)' : 'Rec Replay (F9)';
      btnPlayReplay.classList.toggle('active', replay.playing);
      btnPlayReplay.textContent = replay.playing ? 'Stop Play (F10)' : 'Play Replay (F10)';
      chkHideVdp1.checked = (dbgLayers & 1) !== 0;
      chkHideRbg0.checked = (dbgLayers & 2) !== 0;
      chkHideNbg.checked = (dbgLayers & 4) !== 0;
      chkHd704.checked = (dbgLayers & 8) !== 0;
      btnVidUseGpu.textContent = prefs.useGpu ? 'WebGL2 GPU: ON' : 'WebGL2 GPU: OFF';
      btnVidUseGpu.classList.toggle('active', prefs.useGpu);
      btnAudRealScsp.textContent = realScspAudio ? 'SCSP Real: ON' : 'SCSP Real: OFF';
      btnAudRealScsp.classList.toggle('active', realScspAudio);
      menuVideoBackend.textContent = `${stats.videoBackend} (${FILTER_NAMES[prefs.filterMode]})`;
      menuAudioInfo.textContent = `AudioWorklet 44.1 kHz ${audioEngine.ready ? 'LISTO' : 'wait'} | M68K ${stats.m68kEnabled ? 'ON' : 'Fallback'}`;
      if (replay.recording) elReplayInfo.textContent = `REC ${replay.frames.length}f`;
      else if (replay.playing) elReplayInfo.textContent = `PLAY ${replay.cursor}/${replay.frames.length}`;
      else if (replay.frames.length > 0) elReplayInfo.textContent = `Ready (${replay.frames.length}f)`;
      else elReplayInfo.textContent = 'Replay: idle';
    }

    syncWorkerSoloRunState();
  }

  function setMainTab(idx) {
    activeMainTab = Math.max(0, Math.min(2, idx | 0));
    listeningTarget = null;
    listeningPadTarget = null;
    padListenBaseline = null;
    if (activeMainTab === 1 && selSigMode.value === 'worker' && inputWorkerUrl.value) {
      window.TwcRtcNet?.prefetchWorkerIce?.(inputWorkerUrl.value);
    } else if (activeMainTab === 2) {
      renderRemapGrids();
      renderPadRemapGrids();
    }
    syncUiState();
  }

  function setDebugMode(nextVal) {
    debugMode = devModeEnabled ? Boolean(nextVal) : false;
    if (worker) worker.postMessage({ type: 'set_debug_mode', enabled: debugMode });
    syncUiState();
  }

  function openModeSelect() {
    if (!stats.ready || netSession.inMatch) return;
    selectedStartCopa = copaMode;
    modeSelectOpen = true;
    paused = true;
    syncUiState();
  }

  async function selectEditionAndStart(targetCopa, forceReboot = false) {
    if (!worker) return;
    const isOnlineSession = netSession.connected || netSession.inMatch || netSession.handshaking;
    if (isOnlineSession) {
      if (netSession.role !== 'host') return;
      ensureAudio();
      copaMode = Boolean(targetCopa);
      prefs.savedCopaMode = copaMode;
      saveJsonPrefs(PREFS_KEY, prefs);
      modeSelectOpen = false;
      syncUiState();
      if (netSession.inMatch || forceReboot) {
        await startOnlineMatchFromHost();
      } else {
        try { rtcClient?.sendCtrl({ t: 'lobby_mode', copaMode, coopMode: onlineCoopMode, hardMode, matchRuleKey }); } catch {}
      }
      return;
    }

    ensureAudio();
    pressedKeys.clear();
    resetTouchInputState();
    copaMode = Boolean(targetCopa);
    prefs.savedCopaMode = copaMode;
    saveJsonPrefs(PREFS_KEY, prefs);

    copaSeed = (Date.now() >>> 0) || 0x1998ca01;
    modeSelectOpen = false;
    loadingKind = forceReboot ? 'restarting' : 'switching';
    paused = true;
    syncUiState();

    try {
      const msg = await sendWorkerReq('switch_edition_clean', {
        copaMode,
        copaSeed,
        gkFix: true,
        hardMode: Boolean(hardMode),
        coopMode: Boolean(localCoopMode),
        ruleMode: 0,
        ruleValue: 90,
        markerFix: false,
        realAudio: realScspAudio,
        dbgLayers,
        forceReboot,
      });
      if (msg) {
        applyTelemetry(msg.telemetry);
        paintFrame(msg);
        if (debugMode) updateCanvasHash();
      }
      paused = false;
      stats.framesRun = 0;
      frameTimes.length = 0;
      renderTimes.length = 0;
      lagTimes.length = 0;
      profMaxSimMs = 0;
      profMaxRenMs = 0;
      profStalls = 0;
      profLastFrameRecvMs = 0;
    } finally {
      loadingKind = null;
      syncUiState();
    }
  }

  function setSpeedIndex(idx) {
    if (netSession.inMatch) return;
    speedIdx = Math.max(0, Math.min(SPEED_PERCENTS.length - 1, idx));
    if (worker) {
      worker.postMessage({ type: 'solo_set_speed', speedPercent: SPEED_PERCENTS[speedIdx] });
    }
    syncUiState();
  }

  function selectSlot(idx) {
    slotIdx = ((idx % 3) + 3) % 3;
    const snap = savedSlots[slotIdx];
    if (elSave) elSave.textContent = snap ? `Slot ${slotIdx}: ${snap.length} B ready` : `Slot ${slotIdx}: empty`;
    syncUiState();
  }

  let onlinePulseSysMask = 0xff;
  let onlinePulseTimer = null;

  function pulsePort(port, mask, frames = 8) {
    ensureAudio();
    if (!worker) return;
    if (netSession.inMatch) {
      if (port === 'c') {
        const isGuest = netSession.localPlayer === 1;
        let mapped = 0xff;
        if ((mask & 0x01) === 0) mapped &= isGuest ? ~0x02 : ~0x01;
        if ((mask & 0x10) === 0) mapped &= isGuest ? ~0x20 : ~0x10;
        onlinePulseSysMask &= mapped;
        if (onlinePulseTimer) clearTimeout(onlinePulseTimer);
        onlinePulseTimer = setTimeout(() => {
          onlinePulseSysMask = 0xff;
          onlinePulseTimer = null;
          pushLiveNetInputToWorker(true);
        }, Math.max(80, Math.round(frames * (1000 / 60))));
        pushLiveNetInputToWorker(true);
      }
      return;
    }
    worker.postMessage({ type: 'solo_pulse_port', port, mask: mask & 0xff, frames });
  }

  function triggerQuickMatch() {
    ensureAudio();
    if (!worker || netSession.inMatch) return;
    modeSelectOpen = false;
    paused = false;
    worker.postMessage({ type: 'solo_quick_match' });
    syncUiState();
  }

  function triggerForceWin() {
    ensureAudio();
    if (!worker || !debugMode || netSession.inMatch) return;
    modeSelectOpen = false;
    paused = false;
    worker.postMessage({ type: 'solo_force_win' });
    syncUiState();
  }

  function isPadBindPressed(gp, bind) {
    if (!gp || !bind) return false;
    if (bind.type === 'axis') {
      const val = Number(gp.axes[bind.index] || 0);
      return bind.dir < 0 ? val < -0.45 : val > 0.45;
    }
    if (gp.buttons[bind.index]?.pressed) return true;
    if (typeof bind.axis === 'number') {
      const val = Number(gp.axes[bind.axis] || 0);
      return bind.dir < 0 ? val < -0.45 : val > 0.45;
    }
    return false;
  }

  function applyPadToPorts(gp, padMap, paOrPb, pc, isP2, isOnlineGuest) {
    if (!gp || !padMap) return { dst: paOrPb, pc };
    let dst = paOrPb;
    if (isPadBindPressed(gp, padMap.btn1)) dst &= ~0x01;
    if (isPadBindPressed(gp, padMap.btn2)) dst &= ~0x02;
    if (isPadBindPressed(gp, padMap.btn3)) dst &= ~0x04;
    if (isPadBindPressed(gp, padMap.btn4)) dst &= ~0x08;
    if (isPadBindPressed(gp, padMap.down)) dst &= ~0x10;
    if (isPadBindPressed(gp, padMap.up)) dst &= ~0x20;
    if (isPadBindPressed(gp, padMap.right)) dst &= ~0x40;
    if (isPadBindPressed(gp, padMap.left)) dst &= ~0x80;
    const startBind = isP2 ? padMap.start2 : padMap.start1;
    const coinBind = isP2 ? padMap.coin2 : padMap.coin1;
    if (isPadBindPressed(gp, startBind)) pc &= (isP2 || isOnlineGuest) ? ~0x20 : ~0x10;
    if (isPadBindPressed(gp, coinBind)) pc &= (isP2 || isOnlineGuest) ? ~0x02 : ~0x01;
    return { dst, pc };
  }

  function readLivePorts() {
    let pa = 0xff;
    let pb = 0xff;
    let pc = 0xff;

    // Player 1 (Defaults: WASD + J/K/L/I + Space/R)
    if (pressedKeys.has(p1KeyMap.btn1)) pa &= ~0x01;
    if (pressedKeys.has(p1KeyMap.btn2)) pa &= ~0x02;
    if (pressedKeys.has(p1KeyMap.btn3)) pa &= ~0x04;
    if (pressedKeys.has(p1KeyMap.btn4)) pa &= ~0x08;
    if (pressedKeys.has(p1KeyMap.down)) pa &= ~0x10;
    if (pressedKeys.has(p1KeyMap.up)) pa &= ~0x20;
    if (pressedKeys.has(p1KeyMap.right)) pa &= ~0x40;
    if (pressedKeys.has(p1KeyMap.left)) pa &= ~0x80;

    // Player 2 Local (Defaults: Arrow keys + Num 1/2/3/5 + 0/9)
    if (pressedKeys.has(p2KeyMap.btn1)) pb &= ~0x01;
    if (pressedKeys.has(p2KeyMap.btn2)) pb &= ~0x02;
    if (pressedKeys.has(p2KeyMap.btn3)) pb &= ~0x04;
    if (pressedKeys.has(p2KeyMap.btn4)) pb &= ~0x08;
    if (pressedKeys.has(p2KeyMap.down)) pb &= ~0x10;
    if (pressedKeys.has(p2KeyMap.up)) pb &= ~0x20;
    if (pressedKeys.has(p2KeyMap.right)) pb &= ~0x40;
    if (pressedKeys.has(p2KeyMap.left)) pb &= ~0x80;

    if (pressedKeys.has(p1KeyMap.coin1) || touchState.coin) pc &= ~0x01;
    if (pressedKeys.has(p2KeyMap.coin2)) pc &= ~0x02;
    if (pressedKeys.has(p1KeyMap.start1) || touchState.start) pc &= ~0x10;
    if (pressedKeys.has(p2KeyMap.start2)) pc &= ~0x20;

    if (touchState.a) pa &= ~0x01;
    if (touchState.b) pa &= ~0x02;
    if (touchState.c) pa &= ~0x04;
    if (touchState.down) pa &= ~0x10;
    if (touchState.up) pa &= ~0x20;
    if (touchState.right) pa &= ~0x40;
    if (touchState.left) pa &= ~0x80;

    const { pad1, pad2 } = getAssignedPadPair();
    if (pad1) {
      const res1 = applyPadToPorts(pad1, p1PadMap, pa, pc, false, false);
      pa = res1.dst;
      pc = res1.pc;
    }
    if (padPrefs.twoPads && pad2) {
      const res2 = applyPadToPorts(pad2, p2PadMap, pb, pc, true, false);
      pb = res2.dst;
      pc = res2.pc;
    }
    return { pa, pb, pc };
  }

  function readLocalNetPacked() {
    // Online 1v1 uses Player 1 control layout exclusively on both Host (P1) and Guest (P2).
    let pa = 0xff;
    if (pressedKeys.has(p1KeyMap.btn1)) pa &= ~0x01;
    if (pressedKeys.has(p1KeyMap.btn2)) pa &= ~0x02;
    if (pressedKeys.has(p1KeyMap.btn3)) pa &= ~0x04;
    if (pressedKeys.has(p1KeyMap.btn4)) pa &= ~0x08;
    if (pressedKeys.has(p1KeyMap.down)) pa &= ~0x10;
    if (pressedKeys.has(p1KeyMap.up)) pa &= ~0x20;
    if (pressedKeys.has(p1KeyMap.right)) pa &= ~0x40;
    if (pressedKeys.has(p1KeyMap.left)) pa &= ~0x80;

    let sysC = 0xff & onlinePulseSysMask;
    const isGuest = netSession.localPlayer === 1;
    if (pressedKeys.has(p1KeyMap.coin1) || touchState.coin) sysC &= isGuest ? ~0x02 : ~0x01;
    if (pressedKeys.has(p1KeyMap.start1) || touchState.start) sysC &= isGuest ? ~0x20 : ~0x10;

    if (touchState.a) pa &= ~0x01;
    if (touchState.b) pa &= ~0x02;
    if (touchState.c) pa &= ~0x04;
    if (touchState.down) pa &= ~0x10;
    if (touchState.up) pa &= ~0x20;
    if (touchState.right) pa &= ~0x40;
    if (touchState.left) pa &= ~0x80;

    const { pad1 } = getAssignedPadPair();
    if (pad1) {
      const res1 = applyPadToPorts(pad1, p1PadMap, pa, sysC, false, isGuest);
      pa = res1.dst;
      sysC = res1.pc;
    }
    return { buttons: pa & 0xff, sysC: sysC & 0xff };
  }

  function pushLiveSoloInputToWorker(force = false) {
    if (!worker || netSession.inMatch || paused || modeSelectOpen) return;
    const { pa, pb, pc } = readLivePorts();
    const dash = Boolean(touchState.dash);
    if (!force && pa === lastSentSoloPa && pb === lastSentSoloPb && pc === lastSentSoloPc && dash === lastSentSoloDash) {
      return;
    }
    lastSentSoloPa = pa;
    lastSentSoloPb = pb;
    lastSentSoloPc = pc;
    lastSentSoloDash = dash;
    worker.postMessage({ type: 'solo_set_input', pa, pb, pc, dash });
  }

  function pushLiveNetInputToWorker(force = false) {
    if (!worker || !netSession.inMatch) return;
    const { buttons, sysC } = readLocalNetPacked();
    const dash = Boolean(touchState.dash);
    if (!force && buttons === lastSentLiveButtons && sysC === lastSentLiveSysC && dash === lastSentLiveDash) {
      return;
    }
    lastSentLiveButtons = buttons;
    lastSentLiveSysC = sysC;
    lastSentLiveDash = dash;
    worker.postMessage({ type: 'set_live_input', localButtons: buttons, localSysC: sysC, dash });
  }

  function stepOneWorkerFrame() {
    if (!worker || !stats.ready || netSession.inMatch) return;
    ensureAudio();
    paused = true;
    modeSelectOpen = false;
    const { pa, pb, pc } = readLivePorts();
    worker.postMessage({ type: 'solo_step_one', pa, pb, pc });
    syncUiState();
  }

  let lastHashFrame = 0;

  function tick() {
    const rafNow = performance.now();
    if (vsyncF64) vsyncF64[0] = rafNow;
    if (!mtActive && (needsPresent || gpuPktQueue.length > 0)) {
      let didPresent = false;
      if (gpuPktQueue.length > 0) {
        const item = gpuPktQueue.shift();
        needsPresent = (gpuPktQueue.length > 0);
        if (item && item.buf && item.buf.byteLength > 0 && item.len > 0) {
          const tg0 = profEnabled ? performance.now() : 0;
          presenter.presentGpuVdp2(item.u8 || new Uint8Array(item.buf, 0, item.len), item.i32 || null);
          if (profEnabled) {
            glTimes.push(performance.now() - tg0);
            if (glTimes.length > 120) glTimes.shift();
          }
          recycleGpuPktBuf(item.buf);
          didPresent = true;
        }
      } else if (pendingBandFrame && bandSabU8) {
        const bf = pendingBandFrame;
        pendingBandFrame = null;
        needsPresent = false;
        const byteLen = bf.w * bf.h * 4;
        if (byteLen > 0) {
          if (!bandDisplayBuf || bandDisplayBuf.byteLength !== byteLen) {
            bandDisplayBuf = new Uint8Array(byteLen);
          }
          const srcOff = BAND_RGBA_OFFSET0 + (bf.slot & 1) * BAND_RGBA_STRIDE;
          bandDisplayBuf.set(bandSabU8.subarray(srcOff, srcOff + byteLen));
          lastRgba = bandDisplayBuf;
          lastW = bf.w;
          lastH = bf.h;
          presenter.present(bandDisplayBuf, bf.w, bf.h);
          didPresent = true;
        }
      } else if (lastRgba) {
        needsPresent = false;
        presenter.present(lastRgba, lastW, lastH);
        didPresent = true;
      }
      if (didPresent) {
        if (profVisWinStart === 0) profVisWinStart = rafNow;
        profVisWinCount++;
        if (rafNow - profVisWinStart >= 950) {
          profVisFps = Number(((profVisWinCount * 1000) / (rafNow - profVisWinStart)).toFixed(1));
          profVisWinStart = rafNow;
          profVisWinCount = 0;
        }
      }
    }

    if (!detectedMobile || anyGamepadSeen || listeningPadTarget || ((padPollCounter++ % 60) === 0)) {
      const curPads = getConnectedPads();
      if (curPads.length > 0) anyGamepadSeen = true;
      const curPadKey = curPads.map((p) => `${p.index}:${p.id}`).join('|');
      if (curPadKey !== lastPadDetectKey) {
        lastPadDetectKey = curPadKey;
        renderPadRemapGrids();
        syncUiState();
      }
    }
    if (listeningPadTarget) {
      pollGamepadBindCapture();
    }

    if (stats.ready && !stats.halted) {
      if (netSession.inMatch) {
        pushLiveNetInputToWorker(false);
        if (stats.framesRun - lastHashFrame >= 15) {
          lastHashFrame = stats.framesRun;
          updateHudTelemetry();
        }
      } else if (!paused && !modeSelectOpen && !netSession.handshaking) {
        pushLiveSoloInputToWorker(false);
        if (stats.framesRun - lastHashFrame >= 30) {
          lastHashFrame = stats.framesRun;
          updateHudTelemetry();
          if (debugMode) updateCanvasHash();
        }
      }
    }
    requestAnimationFrame(tick);
  }

  async function doQuickSave(slot = slotIdx) {
    ensureAudio();
    if (!worker || netSession.inMatch) return false;
    const res = await sendWorkerReq('save_snapshot', { slot, purpose: 'slot' });
    if (!res || !res.snapshot) {
      if (elSave) elSave.textContent = `Slot ${slot}: SAVE FAILED`;
      return false;
    }
    const copy = new Uint8Array(res.snapshot);
    savedSlots[slot] = copy;
    const hHex = await sha256Hex(copy);
    stats.snapshotBytes = copy.length;
    stats.snapshotSha = hHex;
    if (elSave) elSave.textContent = `S${slot} SAVED ${copy.length} B (${hHex.slice(0, 12)}...)`;
    return true;
  }

  async function doQuickLoad(slot = slotIdx) {
    ensureAudio();
    if (!worker || netSession.inMatch) return false;
    const snap = savedSlots[slot];
    if (!snap) {
      if (elSave) elSave.textContent = `Slot ${slot}: EMPTY`;
      return false;
    }
    const copy = snap.slice();
    const res = await sendWorkerReq('load_snapshot', { slot, purpose: 'slot', snapshot: copy.buffer }, [copy.buffer]);
    stats.saveLoadOk = Boolean(res && res.ok);
    if (stats.saveLoadOk) {
      if (debugMode) await updateCanvasHash();
      if (elSave) elSave.textContent = `S${slot} RESTORED (${(stats.snapshotSha || '').slice(0, 12)}...)`;
    } else if (elSave) {
      elSave.textContent = `Slot ${slot}: LOAD FAILED`;
    }
    return stats.saveLoadOk;
  }

  async function toggleRecordReplay() {
    ensureAudio();
    if (!worker || netSession.inMatch) return false;
    if (replay.playing) {
      replay.playing = false;
      worker.postMessage({ type: 'solo_set_replay', active: false });
    }
    if (replay.recording) {
      replay.recording = false;
      syncUiState();
      return true;
    }
    const res = await sendWorkerReq('save_snapshot', { purpose: 'replay' });
    if (!res || !res.snapshot) return false;
    replay.initSnapshot = new Uint8Array(res.snapshot);
    replay.frames = [];
    replay.cursor = 0;
    replay.recording = true;
    syncUiState();
    return true;
  }

  async function togglePlayReplay() {
    ensureAudio();
    if (!worker || netSession.inMatch) return false;
    if (replay.recording) replay.recording = false;
    if (replay.playing) {
      replay.playing = false;
      worker.postMessage({ type: 'solo_set_replay', active: false });
      syncUiState();
      return true;
    }
    if (!replay.initSnapshot || replay.frames.length === 0) {
      if (elReplayInfo) elReplayInfo.textContent = 'No replay recorded';
      return false;
    }
    const copy = replay.initSnapshot.slice();
    const res = await sendWorkerReq('load_snapshot', { purpose: 'replay', snapshot: copy.buffer }, [copy.buffer]);
    if (!res || !res.ok) return false;
    const packed = new Uint8Array(replay.frames.length * 3);
    for (let i = 0; i < replay.frames.length; i++) {
      const rf = replay.frames[i];
      packed[i * 3] = rf.a & 0xff;
      packed[i * 3 + 1] = rf.b & 0xff;
      packed[i * 3 + 2] = rf.c & 0xff;
    }
    worker.postMessage({ type: 'solo_set_replay', active: true, framesBuf: packed.buffer }, [packed.buffer]);
    replay.cursor = 0;
    replay.playing = true;
    paused = false;
    stats.framesRun = 0;
    frameTimes.length = 0;
    renderTimes.length = 0;
    lagTimes.length = 0;
    profMaxSimMs = 0;
    profMaxRenMs = 0;
    profStalls = 0;
    profLastFrameRecvMs = 0;
    if (debugMode) await updateCanvasHash();
    syncUiState();
    return true;
  }

  function exportReplayBytes() {
    if (!replay.initSnapshot) return null;
    const snapLen = replay.initSnapshot.length;
    const fCnt = replay.frames.length;
    const out = new Uint8Array(12 + snapLen + fCnt * 4);
    const dv = new DataView(out.buffer);
    out[0] = 0x54; out[1] = 0x57; out[2] = 0x43; out[3] = 0x52;
    dv.setUint32(4, snapLen, true);
    dv.setUint32(8, fCnt, true);
    out.set(replay.initSnapshot, 12);
    let off = 12 + snapLen;
    for (let i = 0; i < fCnt; i++) {
      const f = replay.frames[i];
      out[off++] = f.a & 0xff;
      out[off++] = f.b & 0xff;
      out[off++] = f.c & 0xff;
      out[off++] = 0;
    }
    return out;
  }

  function importReplayBytes(raw) {
    if (!raw || raw.byteLength < 12) return false;
    if (raw[0] !== 0x54 || raw[1] !== 0x57 || raw[2] !== 0x43 || raw[3] !== 0x52) return false;
    const dv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    const snapLen = dv.getUint32(4, true);
    const fCnt = dv.getUint32(8, true);
    if (12 + snapLen + fCnt * 4 > raw.byteLength) return false;
    replay.recording = false;
    replay.playing = false;
    replay.initSnapshot = new Uint8Array(raw.slice(12, 12 + snapLen));
    replay.frames = [];
    let off = 12 + snapLen;
    for (let i = 0; i < fCnt; i++) {
      replay.frames.push({ a: raw[off], b: raw[off + 1], c: raw[off + 2] });
      off += 4;
    }
    replay.cursor = 0;
    syncUiState();
    return true;
  }

  async function runBench600() {
    if (!worker || netSession.inMatch) return null;
    const wasPaused = paused;
    paused = true;
    syncWorkerSoloRunState();
    const msg = await sendWorkerReq('bench_600');
    stats.bench600 = { ...msg.bench, endCanvasSha: stats.canvasSha };
    if (elBench) elBench.textContent = `${msg.bench.fps} fps (${msg.bench.totalMs} ms, ${msg.bench.realtimeRatio}x)`;
    paused = wasPaused;
    updateHudTelemetry();
    return stats.bench600;
  }

  function setLayerMask(mask) {
    if (!worker) return;
    dbgLayers = mask & 0x0f;
    prefs.hd704 = (dbgLayers & 0x08) !== 0;
    saveJsonPrefs(PREFS_KEY, prefs);
    worker.postMessage({ type: 'set_mode', dbgLayers });
  }

  function toggleEdition() {
    if (!worker || (netSession.connected && netSession.role !== 'host')) return;
    selectEditionAndStart(!copaMode, false);
  }

  function cycleFilterMode() {
    prefs.filterMode = (prefs.filterMode + 1) % 4;
    saveJsonPrefs(PREFS_KEY, prefs);
    applyVideoSettings();
    syncUiState();
  }

  function toggleMute() {
    prefs.muted = !prefs.muted;
    saveJsonPrefs(PREFS_KEY, prefs);
    audioEngine.setMuted(prefs.muted);
    if (!prefs.muted) ensureAudio();
    syncUiState();
  }

  function toggleFullscreen() {
    const target = screenWrap || canvas;
    if (!document.fullscreenElement) {
      if (target.requestFullscreen) target.requestFullscreen().catch(() => {});
    } else if (document.exitFullscreen) {
      document.exitFullscreen().catch(() => {});
    }
  }

  // ---- ONLINE P2P LOBBY & MATCH CONTROL ----
  function resolveChosenDelay() {
    const val = selNetDelay.value;
    if (val === 'auto') return netSession.recommendedDelay || 3;
    return Math.max(1, Math.min(6, parseInt(val, 10) || 3));
  }

  function saveLocalNetPrefs() {
    try {
      localStorage.setItem('twc98_worker_url', inputWorkerUrl.value.trim());
      localStorage.setItem('twc98_turn_url', inputTurnUrl.value.trim());
      localStorage.setItem('twc98_turn_user', inputTurnUser.value.trim());
    } catch {}
  }

  function loadLocalNetPrefs() {
    try {
      const qs = new URLSearchParams(window.location.search);
      const hs = new URLSearchParams((window.location.hash || '').slice(1));
      const sigUrl = hs.get('signal') || qs.get('signal') || localStorage.getItem('twc98_worker_url') || DEFAULT_SIGNAL_WORKER_URL;
      const room = hs.get('room') || qs.get('room') || '';
      if (sigUrl) {
        inputWorkerUrl.value = sigUrl;
        selSigMode.value = 'worker';
        if (room) window.TwcRtcNet?.prefetchWorkerIce?.(sigUrl);
      }
      inputRoomCode.value = room.toUpperCase();
      inputTurnUrl.value = localStorage.getItem('twc98_turn_url') || '';
      inputTurnUser.value = localStorage.getItem('twc98_turn_user') || '';
    } catch {}
  }

  function createRtcClient() {
    saveLocalNetPrefs();
    const client = new window.TwcRtcNet.RtcP2PClient({
      onStatus: ({ text }) => {
        setOnlineLogText(text);
        syncUiState();
      },
      onRtt: ({ avgRttMs, jitterMs, recommendedDelay }) => {
        netSession.rttMs = avgRttMs;
        netSession.jitterMs = jitterMs;
        netSession.recommendedDelay = recommendedDelay;
        if (!netSession.inMatch) updateHudTelemetry();
      },
      onOpen: () => {
        netSession.connected = true;
        modeSelectOpen = false;
        if (netSession.role === 'host') {
          setOnlineLogKey('onlinePeerHostMsg');
          try { client.sendCtrl({ t: 'lobby_mode', copaMode, coopMode: onlineCoopMode, hardMode, matchRuleKey }); } catch {}
        } else {
          setOnlineLogKey('onlinePeerGuestMsg');
        }
        syncUiState();
      },
      onInputPacket: (buf) => {
        if (worker && netSession.inMatch && !netSession.handshaking) {
          worker.postMessage({ type: 'net_rx', wire: buf }, [buf]);
        }
      },
      onControl: async (msg) => {
        if (msg.t === 'lobby_mode' && netSession.role === 'guest' && !netSession.inMatch) {
          copaMode = Boolean(msg.copaMode);
          if (typeof msg.coopMode === 'boolean') onlineCoopMode = msg.coopMode;
          if (typeof msg.hardMode === 'boolean') hardMode = msg.hardMode;
          if (typeof msg.matchRuleKey === 'string') matchRuleKey = msg.matchRuleKey;
          syncUiState();
        } else if (msg.t === 'start_prep' && netSession.role === 'guest') {
          try {
            if (msg.bundleId && bundleId && msg.bundleId !== bundleId) {
              netSession.handshaking = false;
              rtcClient?.sendCtrl({ t: 'prep_ack', guestHash: 'VERSION_MISMATCH', bundleId });
              setOnlineLogText(`Versión distinta: Host=${msg.bundleId.slice(0,8)} vs Local=${bundleId.slice(0,8)} (recarga la página)`);
              syncUiState();
              return;
            }
            netSession.handshaking = true;
            netSession.inMatch = false;
            paused = true;
            syncWorkerSoloRunState();
            netSession.delayFrames = msg.delayFrames || 3;
            netSession.localPlayer = 1;
            copaMode = Boolean(msg.copaMode);
            onlineCoopMode = Boolean(msg.coopMode);
            copaSeed = msg.copaSeed >>> 0;
            gkFixEnabled = true;
            if (typeof msg.hardMode === 'boolean') hardMode = msg.hardMode;
            if (typeof msg.matchRuleKey === 'string') matchRuleKey = msg.matchRuleKey;
            const parsedRule = (typeof msg.ruleMode === 'number' && typeof msg.ruleValue === 'number')
              ? { ruleMode: msg.ruleMode | 0, ruleValue: msg.ruleValue | 0 }
              : parseMatchRuleKey(matchRuleKey);
            if (typeof msg.realAudio === 'boolean') realScspAudio = msg.realAudio;
            modeSelectOpen = false;
            setOnlineLogKey('onlinePrepMsg');
            syncUiState();
            const prep = await sendWorkerReq('handshake_prep', {
              localPlayer: 1,
              delayFrames: netSession.delayFrames,
              copaMode,
              coopMode: onlineCoopMode,
              copaSeed,
              gkFix: true,
              hardMode: Boolean(hardMode),
              ruleMode: parsedRule.ruleMode,
              ruleValue: parsedRule.ruleValue,
              markerFix: false,
              realAudio: realScspAudio,
              idleSkip: (typeof msg.idleSkip === 'boolean') ? msg.idleSkip : Boolean(idleSkipEnabled),
              fastCpu: (typeof msg.fastCpu === 'boolean') ? msg.fastCpu : Boolean(fastCpuEnabled),
              lowRes224p: Boolean(prefs.lowRes224p),
              dbgLayers,
              armNet: true,
            });
            const guestHash = prep.telemetry.syncHash64;
            rtcClient?.sendCtrl({ t: 'prep_ack', guestHash });
            if (guestHash !== msg.hostHash) {
              netSession.handshaking = false;
              await sendWorkerReq('net_stop');
              setOnlineLogText(`DESYNC LOBBY: Host=${msg.hostHash} != Guest=${guestHash}`);
            } else if (debugMode) {
              setOnlineLogText(`Sync Frame 0 OK (${guestHash})`);
            }
          } catch (err) {
            netSession.handshaking = false;
            setOnlineLogText(`Handshake error: ${err.message}`);
          }
        } else if (msg.t === 'prep_ack' && netSession.role === 'host') {
          if (netSession.pendingPrepResolve) {
            netSession.pendingPrepResolve(msg);
            netSession.pendingPrepResolve = null;
          }
        } else if (msg.t === 'go_frame0' && netSession.role === 'guest') {
          loggedDesyncFrame = 0;
          netSession.handshaking = false;
          netSession.inMatch = true;
          paused = false;
          modeSelectOpen = false;
          stats.framesRun = 0;
          lastHashFrame = 0;
          frameTimes.length = 0;
          lastSentLiveButtons = -1;
          lastSentLiveSysC = -1;
          onlinePulseSysMask = 0xff;
          if (onlinePulseTimer) { clearTimeout(onlinePulseTimer); onlinePulseTimer = null; }
          audioEngine.clear();
          pushLiveNetInputToWorker(true);
          syncWorkerSoloRunState();
          worker.postMessage({ type: 'net_start_loop' });
          setOnlineLogKey(onlineCoopMode ? 'onlinePlayingCoopGuestMsg' : 'onlinePlayingGuestMsg');
          syncUiState();
          updateHudTelemetry();
        } else if (msg.t === 'leave') {
          leaveOnlineSession(t('onlinePeerLeftMsg'));
        }
      },
      onDisconnect: (st) => {
        if (netSession.connected || netSession.inMatch) {
          leaveOnlineSession(`P2P (${st})`);
        }
      },
    });
    client.setTurnConfig({
      turnUrl: inputTurnUrl.value,
      turnUser: inputTurnUser.value,
      turnCred: inputTurnCred.value,
    });
    return client;
  }

  function generateRoomCode() {
    const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
    const bytes = new Uint8Array(6);
    if (window.crypto && window.crypto.getRandomValues) {
      window.crypto.getRandomValues(bytes);
    } else {
      for (let i = 0; i < 6; i++) bytes[i] = Math.floor(Math.random() * 256);
    }
    let out = '';
    for (let i = 0; i < 6; i++) out += alphabet[bytes[i] % alphabet.length];
    return out;
  }

  async function startHostRoom() {
    ensureAudio();
    leaveOnlineSession();
    setMainTab(1);
    const curCode = (inputRoomCode.value || '').trim().toUpperCase();
    const roomCode = (!devModeEnabled || !curCode) ? generateRoomCode() : curCode;
    inputRoomCode.value = roomCode;
    netSession.role = 'host';
    netSession.localPlayer = 0;
    rtcClient = createRtcClient();
    syncUiState();
    try {
      await rtcClient.hostStart({
        mode: selSigMode.value,
        room: roomCode,
        workerUrl: inputWorkerUrl.value,
        onManualOffer: (tok) => { txtManualOut.value = tok; },
      });
    } catch (err) {
      setOnlineLogText(`Host error: ${err.message}`);
      leaveOnlineSession();
    }
  }

  async function joinAsGuest() {
    ensureAudio();
    const mode = selSigMode.value;
    const manualTok = txtManualIn.value.trim();
    const roomCode = (inputRoomCode.value || '').trim().toUpperCase();
    if (!roomCode && mode !== 'manual') {
      setOnlineLogText('Ingresa el código de sala del Host para unirte.');
      inputRoomCode.focus();
      return;
    }
    inputRoomCode.value = roomCode;
    if (mode === 'manual' && !manualTok) {
      setOnlineLogKey('onlineTokenErr');
      return;
    }
    leaveOnlineSession();
    setMainTab(1);
    netSession.role = 'guest';
    netSession.localPlayer = 1;
    rtcClient = createRtcClient();
    syncUiState();
    try {
      await rtcClient.guestJoin({
        mode,
        room: roomCode,
        workerUrl: inputWorkerUrl.value,
        manualOfferToken: manualTok,
        onManualAnswer: (tok) => { txtManualOut.value = tok; },
      });
    } catch (err) {
      setOnlineLogText(`Guest error: ${err.message}`);
      leaveOnlineSession();
    }
  }

  async function startOnlineMatchFromHost() {
    if (!rtcClient || !netSession.connected || netSession.role !== 'host') return;
    ensureAudio();
    try {
      loggedDesyncFrame = 0;
      netSession.handshaking = true;
      netSession.inMatch = false;
      paused = true;
      syncWorkerSoloRunState();
      const delayFrames = resolveChosenDelay();
      netSession.delayFrames = delayFrames;
      netSession.localPlayer = 0;
      modeSelectOpen = false;
      copaSeed = (Date.now() >>> 0) || 0x1998ca01;
      const parsedRule = parseMatchRuleKey(matchRuleKey);
      setOnlineLogKey('onlineSyncingMsg');
      syncUiState();

      const prep = await sendWorkerReq('handshake_prep', {
        localPlayer: 0,
        delayFrames,
        copaMode,
        coopMode: onlineCoopMode,
        copaSeed,
        gkFix: true,
        hardMode: Boolean(hardMode),
        ruleMode: parsedRule.ruleMode,
        ruleValue: parsedRule.ruleValue,
        markerFix: false,
        realAudio: realScspAudio,
        idleSkip: Boolean(idleSkipEnabled),
        fastCpu: Boolean(fastCpuEnabled),
        lowRes224p: Boolean(prefs.lowRes224p),
        dbgLayers,
        armNet: true,
      });
      const hostHash = prep.telemetry.syncHash64;

      const ackPromise = new Promise((resolve, reject) => {
        const tm = setTimeout(() => {
          netSession.pendingPrepResolve = null;
          reject(new Error('Guest ACK timeout'));
        }, 12000);
        netSession.pendingPrepResolve = (ack) => {
          clearTimeout(tm);
          resolve(ack);
        };
      });

      rtcClient.sendCtrl({
        t: 'start_prep',
        bundleId: bundleId || '',
        delayFrames,
        copaMode,
        coopMode: onlineCoopMode,
        copaSeed,
        gkFix: gkFixEnabled,
        hardMode: Boolean(hardMode),
        matchRuleKey,
        ruleMode: parsedRule.ruleMode,
        ruleValue: parsedRule.ruleValue,
        realAudio: Boolean(realScspAudio),
        idleSkip: Boolean(idleSkipEnabled),
        fastCpu: Boolean(fastCpuEnabled),
        hostHash,
      });

      const ack = await ackPromise;
      if (ack.guestHash !== hostHash) {
        netSession.handshaking = false;
        await sendWorkerReq('net_stop');
        if (ack.guestHash === 'VERSION_MISMATCH') {
          const remoteVer = String(ack.bundleId || '?').slice(0, 8);
          const localVer = String(bundleId || '?').slice(0, 8);
          setOnlineLogText(`Versión distinta: Local=${localVer} vs Guest=${remoteVer} (ambos deben recargar)`);
        } else {
          setOnlineLogText(`DESYNC HANDSHAKE: Host=${hostHash} != Guest=${ack.guestHash}`);
        }
        syncUiState();
        return;
      }

      loggedDesyncFrame = 0;
      rtcClient.sendCtrl({ t: 'go_frame0' });
      netSession.handshaking = false;
      netSession.inMatch = true;
      paused = false;
      stats.framesRun = 0;
      lastHashFrame = 0;
      frameTimes.length = 0;
      lastSentLiveButtons = -1;
      lastSentLiveSysC = -1;
      onlinePulseSysMask = 0xff;
      if (onlinePulseTimer) { clearTimeout(onlinePulseTimer); onlinePulseTimer = null; }
      audioEngine.clear();
      pushLiveNetInputToWorker(true);
      syncWorkerSoloRunState();
      worker.postMessage({ type: 'net_start_loop' });
      setOnlineLogKey(onlineCoopMode ? 'onlinePlayingCoopHostMsg' : 'onlinePlayingHostMsg');
      syncUiState();
      updateHudTelemetry();
    } catch (err) {
      netSession.handshaking = false;
      if (worker) worker.postMessage({ type: 'net_stop' });
      setOnlineLogText(`Online start error: ${err.message}`);
      syncUiState();
    }
  }

  function leaveOnlineSession(reasonMsg = null) {
    if (rtcClient) {
      try { rtcClient.sendCtrl({ t: 'leave' }); } catch {}
      rtcClient.close();
      rtcClient = null;
    }
    if (worker && (netSession.inMatch || netSession.handshaking)) {
      worker.postMessage({ type: 'net_stop' });
      worker.postMessage({ type: 'set_coop_mode', coopMode: Boolean(localCoopMode) });
    }
    netSession.connected = false;
    netSession.handshaking = false;
    netSession.inMatch = false;
    netSession.role = 'local';
    netSession.localPlayer = 0;
    netSession.netStats = null;
    loggedDesyncFrame = 0;
    onlinePulseSysMask = 0xff;
    if (onlinePulseTimer) { clearTimeout(onlinePulseTimer); onlinePulseTimer = null; }
    if (!modeSelectOpen) paused = false;
    if (reasonMsg) setOnlineLogText(reasonMsg);
    else setOnlineLogKey('onlineOfflineMsg');
    syncUiState();
  }

  // ---- WORKER INIT & CRYPTO BOOT ----
  function spawnWorkerAndAttach() {
    worker = new Worker('emu_worker.js');
    worker.onmessage = (ev) => {
      const msg = ev.data || {};
      if (msg.type === 'boot_progress') {
        const pct = Math.min(100, Math.round((msg.frame / Math.max(1, msg.total)) * 100));
        const totalPct = 50 + Math.round(pct * 0.5);
        accessBar.style.width = `${totalPct}%`;
        accessStatus.textContent = devModeEnabled
          ? `ST-V Cold Boot: ${msg.frame}/${msg.total} (${pct}%)`
          : `${t('vaultLoading')} (${totalPct}%)`;
        return;
      }
      if (msg.type === 'error') {
        stats.error = msg.message;
        stageStatusText.textContent = `ERR: ${msg.message}`;
        accessStatus.textContent = `Error: ${msg.message}`;
        if (msg.reqId && pendingReqs.has(msg.reqId)) {
          const p = pendingReqs.get(msg.reqId);
          pendingReqs.delete(msg.reqId);
          p.reject(new Error(msg.message));
        }
        return;
      }
      if (msg.reqId && pendingReqs.has(msg.reqId)) {
        const p = pendingReqs.get(msg.reqId);
        pendingReqs.delete(msg.reqId);
        p.resolve(msg);
      }

      if (msg.type === 'ready') {
        stats.initMode = msg.initMode || 'cold';
        stats.coldInitMs = msg.coldInitMs || 0;
        stats.wasmBinaryBytes = msg.wasmBinaryBytes || 0;
        applyVideoSettings();
        applyTelemetry(msg.telemetry);
        paintFrame(msg);
        stats.ready = true;
        accessOverlay.classList.add('hidden');
        selectedStartCopa = false; // Default Classic '98 on startup card
        modeSelectOpen = true;
        paused = true;
        syncUiState();
        updateCanvasHash();
        updateHudTelemetry();
        return;
      }

      if (msg.type === 'force_win_ack') {
        if (elSave) {
          elSave.textContent = msg.ok
            ? `Debug Win: ${msg.winGoals}-${msg.p2Goals} (t=1s)`
            : `Debug Win: ${msg.reason || 'Solo en partido'}`;
        }
        return;
      }

      if (msg.type === 'frame') {
        if (msg.txWire && rtcClient && netSession.inMatch && !netSession.handshaking) {
          rtcClient.sendInputBytes(msg.txWire);
        }
        if (msg.netStats) {
          netSession.netStats = msg.netStats;
          if (msg.netStats.desyncs > 0 && msg.netStats.firstDesyncFrame > 0 && loggedDesyncFrame !== msg.netStats.firstDesyncFrame) {
            loggedDesyncFrame = msg.netStats.firstDesyncFrame;
            console.warn(`[TWC98 NET DESYNC] frame=${msg.netStats.firstDesyncFrame} local=${msg.netStats.firstDesyncLocal} remote=${msg.netStats.firstDesyncRemote}`);
            setOnlineLogText(`DESYNC f=${msg.netStats.firstDesyncFrame} (${msg.netStats.firstDesyncLocal} != ${msg.netStats.firstDesyncRemote})`);
          }
        }
        if (typeof msg.simMs === 'number' && (!netSession.inMatch || msg.advanced)) {
          frameTimes.push(msg.simMs);
          if (frameTimes.length > 120) frameTimes.shift();
          if (msg.simMs > profMaxSimMs && stats.framesRun > 5) profMaxSimMs = msg.simMs;
        }
        if (typeof msg.renderMs === 'number' && (msg.rgba || msg.gpuPkt || msg.bandDispatched || (mtActive && !msg.skippedRender)) && (!netSession.inMatch || msg.advanced)) {
          renderTimes.push(msg.renderMs);
          if (renderTimes.length > 120) renderTimes.shift();
          if (msg.renderMs > profMaxRenMs && stats.framesRun > 5) profMaxRenMs = msg.renderMs;
        }
        if (!paused && !modeSelectOpen && !netSession.handshaking && (!netSession.inMatch || msg.advanced)) {
          const rxNow = performance.now();
          if (profLastFrameRecvMs > 0 && (rxNow - profLastFrameRecvMs) > 80) {
            profStalls++;
          }
          profLastFrameRecvMs = rxNow;
        } else {
          profLastFrameRecvMs = 0;
        }
        if (typeof msg.lagMs === 'number' && (!netSession.inMatch || msg.advanced)) {
          lagTimes.push(msg.lagMs);
          if (lagTimes.length > 120) lagTimes.shift();
        }
        if (msg.skippedRender) profSkippedFrames++;
        else if (!netSession.inMatch || msg.advanced) profRenderFrames++;
        if (!netSession.inMatch || msg.advanced) {
          stats.framesRun++;
          if (profEnabled && !paused && !modeSelectOpen && !netSession.handshaking) {
            const nowMs = performance.now();
            if (profLastTickMs > 0) {
              const dtMs = nowMs - profLastTickMs;
              if (dtMs > 0 && dtMs <= 250) profActiveMs += dtMs;
            }
            profLastTickMs = nowMs;
            if (profBaseSimMs === null && frameTimes.length >= 60 && renderTimes.length >= 15) {
              const sSorted = [...frameTimes].sort((a, b) => a - b);
              const rSorted = [...renderTimes].sort((a, b) => a - b);
              const sMed = sSorted[Math.floor(sSorted.length * 0.5)];
              const rMed = rSorted[Math.floor(rSorted.length * 0.5)];
              if ((sMed >= 1.2 && profActiveMs >= 12000) || profActiveMs >= 35000) {
                profBaseSimMs = Number(sMed.toFixed(2));
                profBaseRenMs = Number(rMed.toFixed(2));
              }
            }
          }
        }
        if (!netSession.inMatch && msg.usedPorts) {
          if (replay.recording) {
            replay.frames.push({ a: msg.usedPorts.a, b: msg.usedPorts.b, c: msg.usedPorts.c });
          }
          if (replay.playing) {
            replay.cursor++;
            if (msg.replayFinished) {
              replay.playing = false;
              syncUiState();
            }
          }
        }
        if (msg.pacerState) {
          const prevWarn = Boolean(pacerState.below30Warning);
          pacerState = msg.pacerState;
          if (pacerState.autoLowResRequested && !gpuVdpActive && detectedMobile && !prefs.lowRes224p) {
            prefs.lowRes224p = true;
            syncUiState();
          } else if (prevWarn !== Boolean(pacerState.below30Warning)) {
            syncUiState();
          }
        }
        applyTelemetry(msg.telemetry);
        if (msg.gpuPkt || msg.rgba) paintFrame(msg, !paused && !modeSelectOpen);
        playWorkerAudio(msg);
        if (netSession.inMatch && msg.advanced && (stats.framesRun % 15) === 0) {
          updateHudTelemetry();
        }
        return;
      }

      if (msg.type === 'pacer_state' && msg.pacerState) {
        pacerState = msg.pacerState;
        syncUiState();
        return;
      }

      if (msg.type === 'mode_ack' || msg.type === 'snapshot_loaded' || msg.type === 'handshake_ready' || msg.type === 'bench_done' || msg.type === 'edition_ready') {
        if (msg.pacerState) pacerState = msg.pacerState;
        if (msg.type === 'edition_ready' || msg.type === 'snapshot_loaded') profLastTickMs = 0;
        applyTelemetry(msg.telemetry);
        if (msg.gpuPkt || msg.rgba) paintFrame(msg);
        if (msg.type === 'mode_ack' || msg.type === 'edition_ready') {
          if (debugMode) updateCanvasHash();
          syncUiState();
        }
      }
    };
  }

  function computeDesiredBandCount() {
    if (mtActive || typeof SharedArrayBuffer === 'undefined') return 0;
    if (window.TwcPerfProfile && typeof window.TwcPerfProfile.computeBandWorkerCount === 'function') {
      return window.TwcPerfProfile.computeBandWorkerCount({
        isMobile: detectedMobile,
        cores: deviceProfile.cores,
        deviceMemoryGb: deviceProfile.deviceMemoryGb,
        explicitParam: bandsParam,
      });
    }
    return 0;
  }

  function startBandPool(wasmBytesSrc) {
    if (bandPoolActive) return { bandSab, emuBandPorts: [] };
    const nBands = computeDesiredBandCount();
    if (nBands < 2 || !wasmBytesSrc) return { bandSab: null, emuBandPorts: [] };
    try {
      bandSab = new SharedArrayBuffer(BAND_SAB_BYTES);
      bandHdr = new Int32Array(bandSab, 0, 16);
      bandSabU8 = new Uint8Array(bandSab);
      Atomics.store(bandHdr, 0, 0);
      Atomics.store(bandHdr, 1, 0);
      Atomics.store(bandHdr, 2, nBands);
      Atomics.store(bandHdr, 3, 0);
      Atomics.store(bandHdr, 6, 0);
      Atomics.store(bandHdr, 8, BAND_PKT_OFFSET);
      Atomics.store(bandHdr, 9, BAND_RGBA_OFFSET0);
      Atomics.store(bandHdr, 10, BAND_RGBA_STRIDE);

      const emuBandPorts = [];
      bandWorkers = [];
      for (let i = 0; i < nBands; i++) {
        const ch = new MessageChannel();
        emuBandPorts.push(ch.port1);
        const bw = new Worker('render_worker.js');
        bw.onmessage = (ev) => {
          const m = ev.data || {};
          if (m.type === 'band_frame_ready' && m.w > 0 && m.h > 0) {
            if (typeof m.renderMs === 'number' && !gpuVdpActive) {
              renderTimes.push(m.renderMs);
              if (renderTimes.length > 120) renderTimes.shift();
              if (m.renderMs > profMaxRenMs && stats.framesRun > 5) profMaxRenMs = m.renderMs;
            }
            if (!paused && !modeSelectOpen) {
              pendingBandFrame = { slot: m.slot & 1, w: m.w | 0, h: m.h | 0 };
              needsPresent = true;
            } else if (bandSabU8) {
              const byteLen = (m.w | 0) * (m.h | 0) * 4;
              if (!bandDisplayBuf || bandDisplayBuf.byteLength !== byteLen) {
                bandDisplayBuf = new Uint8Array(byteLen);
              }
              const srcOff = BAND_RGBA_OFFSET0 + (m.slot & 1) * BAND_RGBA_STRIDE;
              bandDisplayBuf.set(bandSabU8.subarray(srcOff, srcOff + byteLen));
              lastRgba = bandDisplayBuf;
              lastW = m.w | 0;
              lastH = m.h | 0;
              presenter.present(bandDisplayBuf, lastW, lastH);
            }
          } else if (m.type === 'error') {
            stats.error = m.message;
          }
        };
        const wCopy = wasmBytesSrc.slice().buffer;
        bw.postMessage({
          type: 'init_band',
          bandIdx: i,
          bandCount: nBands,
          wasmBytes: wCopy,
          bandSab,
          bandPort: ch.port2,
        }, [wCopy, ch.port2]);
        bandWorkers.push(bw);
      }
      bandPoolActive = true;
      return { bandSab, emuBandPorts };
    } catch {
      bandWorkers.forEach((w) => { try { w.terminate(); } catch {} });
      bandWorkers = [];
      bandSab = null;
      bandSabU8 = null;
      bandHdr = null;
      bandPoolActive = false;
      return { bandSab: null, emuBandPorts: [] };
    }
  }

  async function initEmulatorFromLoadedAssets(loaded) {
    spawnWorkerAndAttach();
    const cacheTag = loaded.cachedBytes > 0
      ? `${(loaded.cachedBytes / 1048576).toFixed(1)} MiB cached`
      : `${(loaded.downloadedBytes / 1048576).toFixed(1)} MiB net`;

    accessBar.style.width = '50%';
    accessSub.textContent = devModeEnabled ? accessSub.textContent : t('vaultLoading');
    accessStatus.textContent = devModeEnabled
      ? (loaded.mode === 'dev_cp' ? 'Initializing Wasm...' : 'Cold Boot ST-V (820 frames)...')
      : `${t('vaultLoading')} (50%)`;

    if (!mtActive && computeDesiredBandCount() >= 2) {
      cachedWasmBytesForBands = loaded.wasmBytes.slice();
    }

    let renderSab = null;
    let emuRenderPort = null;
    if (mtActive) {
      try {
        const offscreen = canvas.transferControlToOffscreen();
        renderSab = new SharedArrayBuffer(64 + 3 * RENDER_SLOT_BYTES);
        const rHdr = new Int32Array(renderSab, 0, 16);
        Atomics.store(rHdr, 11, -1);
        Atomics.store(rHdr, 12, RENDER_SLOT_BYTES);
        const ch = new MessageChannel();
        emuRenderPort = ch.port1;
        const initSz = computeCanvasTargetSize();
        const rwWasmBuf = loaded.wasmBytes.slice().buffer;
        renderWorker = new Worker('render_worker.js');
        renderWorker.onmessage = (ev) => {
          const m = ev.data || {};
          if (m.type === 'ready' && m.backendName) {
            stats.videoBackend = `${m.backendName}-Worker`;
          } else if (m.type === 'vis_fps') {
            renderWorkerVisFps = m.visFps || 0;
            renderWorkerFrames = m.visFrames || 0;
          } else if (m.type === 'error') {
            stats.error = m.message;
          }
        };
        renderWorker.postMessage({
          type: 'init',
          canvas: offscreen,
          wasmBytes: rwWasmBuf,
          renderSab,
          renderPort: ch.port2,
          isMobile: detectedMobile,
          isSoftwareRenderer: detectedSoftwareGpu,
          isIntegratedGpu: detectedIntegratedGpu,
          initW: initSz.w,
          initH: initSz.h,
          filterMode: prefs.filterMode,
          crtMask: Boolean(prefs.crtMask),
          hdMode: Boolean(prefs.hdMode),
          useGpu: Boolean(prefs.useGpu),
        }, [offscreen, rwWasmBuf, ch.port2]);

        const forwardResize = () => presenter.invalidateSize();
        if (typeof ResizeObserver !== 'undefined') {
          new ResizeObserver(forwardResize).observe(canvas);
        }
        window.addEventListener('resize', forwardResize, { passive: true });
      } catch {
        mtActive = false;
        renderWorker = null;
        renderSab = null;
        emuRenderPort = null;
        presenter = new window.TwcWebGLPresenter(canvas);
        applyVideoSettings();
      }
    }

    let initBandSab = null;
    let initBandPorts = [];
    if (!mtActive && !gpuVdpActive && cachedWasmBytesForBands) {
      const bp = startBandPool(cachedWasmBytesForBands);
      initBandSab = bp.bandSab;
      initBandPorts = bp.emuBandPorts;
    }

    // Always prewarm Classic '98 by default so pressing START on the default card is instant
    const audioSab = audioEngine.getSharedAudioSab();
    const payload = {
      type: 'init',
      wasmBytes: loaded.wasmBytes.buffer,
      cart: loaded.cartBytes.buffer,
      bios: loaded.biosBytes.buffer,
      copaMode: false,
      coopMode: Boolean(localCoopMode),
      copaSeed,
      gkFix: true,
      hardMode: Boolean(hardMode),
      ruleMode: 0,
      ruleValue: 90,
      markerFix: false,
      realAudio: realScspAudio,
      dbgLayers,
      lowRes224p: Boolean(prefs.lowRes224p),
      lowResHalfWidth: (!gpuVdpActive && detectedMobile) || (isLowEndGpuOrMobile && initQs.get('render') === '352'),
      warmupFrames: 820,
      idleSkip: idleSkipEnabled,
      audioSab,
      renderSab,
      renderPort: emuRenderPort,
      mt: mtActive,
      gpuVdp: gpuVdpActive,
      gpuVdp1: gpuVdp1Active,
      sh2Aot: sh2AotEnabled,
      fastCpu: fastCpuEnabled,
      fpsMode: prefs.fpsMode || 'auto',
      bandSab: initBandSab,
      bandPorts: initBandPorts,
      vsyncSab,
      isMobile: Boolean(detectedMobile),
    };
    const transfers = [loaded.wasmBytes.buffer, loaded.cartBytes.buffer, loaded.biosBytes.buffer];
    if (emuRenderPort) transfers.push(emuRenderPort);
    for (let i = 0; i < initBandPorts.length; i++) transfers.push(initBandPorts[i]);

    if (loaded.epCp) {
      payload.epCp = loaded.epCp.buffer;
      payload.v1Vram = loaded.v1Vram.buffer;
      payload.v1Regs = loaded.v1Regs.buffer;
      payload.v2Vram = loaded.v2Vram.buffer;
      payload.v2Cram = loaded.v2Cram.buffer;
      payload.v2Regs = loaded.v2Regs.buffer;
      transfers.push(
        loaded.epCp.buffer, loaded.v1Vram.buffer, loaded.v1Regs.buffer,
        loaded.v2Vram.buffer, loaded.v2Cram.buffer, loaded.v2Regs.buffer
      );
    }

    worker.postMessage(payload, transfers);

    const readyCheck = setInterval(() => {
      if (stats.ready) {
        clearInterval(readyCheck);
        if (elCold) elCold.textContent = `${stats.coldInitMs} ms (${stats.initMode}, ${cacheTag})`;
      }
    }, 100);
  }

  function handlePauseToggle() {
    if (!stats.ready || modeSelectOpen || netSession.inMatch || netSession.handshaking) return;
    paused = !paused;
    syncUiState();
  }

  function handleRestartAction() {
    if (!stats.ready || netSession.handshaking) return;
    if (netSession.connected || netSession.inMatch) {
      if (netSession.role !== 'host') return;
      startOnlineMatchFromHost();
      return;
    }
    selectEditionAndStart(copaMode, true);
  }

  function bindUi() {
    loadLocalNetPrefs();
    selLang.value = prefs.lang || 'auto';
    applyTranslations();
    applyVideoSettings();
    audioEngine.setMuted(Boolean(prefs.muted));
    audioEngine.setVolume(prefs.volumePct);

    const gamePreventCodes = new Set(['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Space']);
    const debugPreventCodes = new Set(['Tab', 'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12']);
    const isTypingTarget = (el) => {
      if (!el) return false;
      const tag = (el.tagName || '').toUpperCase();
      return tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT';
    };

    window.addEventListener('pointerdown', () => {
      ensureAudio();
    }, { passive: true, capture: true });

    window.addEventListener('touchstart', () => {
      ensureAudio();
    }, { passive: true, capture: true });

    window.addEventListener('touchend', () => {
      ensureAudio();
    }, { passive: true, capture: true });

    window.addEventListener('pointerup', () => {
      ensureAudio();
    }, { passive: true, capture: true });

    document.addEventListener('visibilitychange', () => {
      if (!document.hidden && !paused && !modeSelectOpen) ensureAudio();
    });

    window.addEventListener('click', (e) => {
      ensureAudio();
      const el = e.target && e.target.closest ? e.target.closest('button, summary') : null;
      if (el && !isTypingTarget(el)) {
        el.blur();
      }
    }, { capture: true });

    window.addEventListener('keydown', (e) => {
      if (listeningPadTarget && e.code === 'Escape') {
        e.preventDefault();
        listeningPadTarget = null;
        padListenBaseline = null;
        renderPadRemapGrids();
        return;
      }
      if (listeningTarget) {
        e.preventDefault();
        if (e.code === 'Escape') {
          listeningTarget = null;
          renderRemapGrids();
          return;
        }
        if (listeningTarget.player === 1) {
          p1KeyMap[listeningTarget.action] = e.code;
          saveJsonPrefs(KEYMAP_P1_KEY, p1KeyMap);
        } else {
          p2KeyMap[listeningTarget.action] = e.code;
          saveJsonPrefs(KEYMAP_P2_KEY, p2KeyMap);
        }
        listeningTarget = null;
        renderRemapGrids();
        syncUiState();
        return;
      }

      if (isTypingTarget(e.target)) return;
      if (e.ctrlKey && e.shiftKey && e.code === 'KeyD') {
        e.preventDefault();
        if (!devModeEnabled) {
          const code = window.prompt('Debug Access Code:');
          if (code) {
            verifyAndUnlockDevCode(code).then((ok) => {
              if (ok) setDebugMode(true);
            });
          }
        } else {
          setDebugMode(!debugMode);
        }
        return;
      }
      ensureAudio();
      pressedKeys.add(e.code);

      if (gamePreventCodes.has(e.code) || (debugMode && debugPreventCodes.has(e.code))) {
        e.preventDefault();
      }

      if (netSession.inMatch) pushLiveNetInputToWorker(false);
      else pushLiveSoloInputToWorker(false);

      if (e.repeat) return;

      // Debug-Only Hotkeys (strictly blocked outside Debug mode)
      if (!debugMode) return;

      if (e.code === 'F1' && !modeSelectOpen) {
        toggleEdition();
      } else if (e.code === 'KeyB') {
        cycleFilterMode();
      } else if (e.code === 'F12') {
        prefs.crtMask = !prefs.crtMask;
        saveJsonPrefs(PREFS_KEY, prefs);
        applyVideoSettings();
        syncUiState();
      } else if (e.code === 'KeyQ') {
        setLayerMask(dbgLayers ^ 0x08);
      } else if (e.code === 'KeyM') {
        toggleMute();
      } else if (e.code === 'F11') {
        toggleFullscreen();
      } else if ((e.code === 'Period' || e.code === 'F6') && !netSession.inMatch) {
        stepOneWorkerFrame();
      } else if (e.code === 'Digit3' || e.code === 'BracketLeft') {
        setSpeedIndex(speedIdx - 1);
      } else if (e.code === 'Digit4' || e.code === 'BracketRight') {
        setSpeedIndex(speedIdx + 1);
      } else if (e.code === 'Tab') {
        selectSlot(slotIdx + 1);
      } else if (e.code === 'F2') {
        setLayerMask(dbgLayers ^ 1);
      } else if (e.code === 'F3') {
        setLayerMask(dbgLayers ^ 2);
      } else if (e.code === 'F4') {
        setLayerMask(dbgLayers ^ 4);
      } else if (e.code === 'F5') {
        doQuickSave(slotIdx);
      } else if (e.code === 'F7') {
        triggerQuickMatch();
      } else if (e.code === 'F8') {
        doQuickLoad(slotIdx);
      } else if (e.code === 'F9') {
        toggleRecordReplay();
      } else if (e.code === 'F10') {
        togglePlayReplay();
      }
    });

    window.addEventListener('keyup', (e) => {
      pressedKeys.delete(e.code);
      if (netSession.inMatch) pushLiveNetInputToWorker(false);
      else pushLiveSoloInputToWorker(false);
    });

    // Header
    selLang.onchange = () => {
      prefs.lang = selLang.value;
      saveJsonPrefs(PREFS_KEY, prefs);
      applyTranslations();
      syncUiState();
      selLang.blur();
    };
    btnDebugToggle.onclick = () => setDebugMode(!debugMode);

    // Startup Mode Select Card
    cardClassicMode.onclick = () => {
      selectedStartCopa = false;
      syncUiState();
    };
    cardCopaMode.onclick = () => {
      selectedStartCopa = true;
      syncUiState();
    };
    btnStartSelectedMode.onclick = () => {
      selectEditionAndStart(selectedStartCopa, false);
    };
    btnStartPlayOnline.onclick = async () => {
      setMainTab(1);
      await selectEditionAndStart(selectedStartCopa, false);
      setMainTab(1);
    };

    // Main Tabs (Menú / Online / Controles)
    mainTabButtons.forEach((b, idx) => {
      b.onclick = () => {
        setMainTab(idx);
      };
    });

    // Footer Game Buttons
    btnCoin1Footer.onclick = () => pulsePort('c', ~0x01, 8);
    btnStart1Footer.onclick = () => pulsePort('c', ~0x10, 8);
    btnFullscreenFooter.onclick = () => toggleFullscreen();
    btnRestartFooter.onclick = () => handleRestartAction();

    // Menú -> Game
    btnMenuPause.onclick = () => { btnMenuPause.blur(); handlePauseToggle(); };
    if (btnScreenPause) {
      btnScreenPause.onclick = () => { btnScreenPause.blur(); handlePauseToggle(); };
    }
    btnMenuRestart.onclick = () => handleRestartAction();
    btnMenuEdClassic.onclick = () => { btnMenuEdClassic.blur(); selectEditionAndStart(false, false); };
    btnMenuEdCopa.onclick = () => { btnMenuEdCopa.blur(); selectEditionAndStart(true, false); };
    if (btnMenuHardMode) {
      btnMenuHardMode.onclick = () => {
        btnMenuHardMode.blur();
        if (netSession.connected || netSession.inMatch || netSession.handshaking) return;
        hardMode = !hardMode;
        prefs.hardMode = hardMode;
        saveJsonPrefs(PREFS_KEY, prefs);
        pushSoloMatchRules();
        syncUiState();
      };
    }
    if (btnMenuLocalCoop) {
      btnMenuLocalCoop.onclick = () => {
        btnMenuLocalCoop.blur();
        if (netSession.connected || netSession.inMatch || netSession.handshaking) return;
        localCoopMode = !localCoopMode;
        if (localCoopMode && !hardMode) {
          hardMode = true;
          prefs.hardMode = true;
          saveJsonPrefs(PREFS_KEY, prefs);
          pushSoloMatchRules();
        }
        pushSoloCoopMode();
        syncUiState();
      };
    }
    if (btnMenuTouchPad) {
      btnMenuTouchPad.onclick = () => {
        btnMenuTouchPad.blur();
        const cur = (prefs.touchPad === 'on' || prefs.touchPad === 'off') ? prefs.touchPad : 'auto';
        prefs.touchPad = cur === 'auto' ? 'on' : (cur === 'on' ? 'off' : 'auto');
        saveJsonPrefs(PREFS_KEY, prefs);
        syncUiState();
      };
    }
    if (profBar) {
      profBar.title = 'Toca para fijar la base térmica (Δ%) al instante actual';
      profBar.onclick = (e) => {
        e.stopPropagation();
        if (frameTimes.length > 0) {
          const sSorted = [...frameTimes].sort((a, b) => a - b);
          profBaseSimMs = Number(sSorted[Math.floor(sSorted.length * 0.5)].toFixed(2));
        }
        if (renderTimes.length > 0) {
          const rSorted = [...renderTimes].sort((a, b) => a - b);
          profBaseRenMs = Number(rSorted[Math.floor(rSorted.length * 0.5)].toFixed(2));
        }
        profActiveMs = 0;
        profLastTickMs = performance.now();
        updateHudTelemetry();
      };
    }

    // Mobile Touch Controls (B-MOB-02 v2: Floating left-half joystick + max-drag double-tap sprint,
    // triangle A/B/C with visual route feedback, chord-grace drag combos, bridge pills, A+B+C, COIN, START)
    const pushTouchPortsNow = () => {
      if (netSession.inMatch) pushLiveNetInputToWorker(false);
      else pushLiveSoloInputToWorker(false);
    };

    const updateJoyFromPoint = (clientX, clientY) => {
      const dx = clientX - joyOrigin.cx;
      const dy = clientY - joyOrigin.cy;
      const maxR = Math.max(26, joyOrigin.radius * 0.96);
      const deadR = Math.max(6, maxR * 0.22);
      // Higher outer-rim threshold so normal dribbling never triggers sprint accidentally
      // (previous values preserved in docs/EXP_SH2_VDP1_CHANGES.md: 0.80 / 0.62)
      const sprintOnR = maxR * 1.12;
      const sprintOffR = maxR * 0.92;
      const dist = Math.hypot(dx, dy);
      const clampScale = dist > maxR ? (maxR / dist) : 1;
      const kx = dx * clampScale;
      const ky = dy * clampScale;
      if (tJoyKnob) {
        tJoyKnob.style.transform = `translate(calc(-50% + ${kx.toFixed(1)}px), calc(-50% + ${ky.toFixed(1)}px))`;
      }
      let up = false, down = false, left = false, right = false;
      if (dist >= deadR) {
        let deg = (Math.atan2(dy, dx) * 180) / Math.PI;
        if (deg < 0) deg += 360;
        if (deg >= 337.5 || deg < 22.5) right = true;
        else if (deg < 67.5) { right = true; down = true; }
        else if (deg < 112.5) down = true;
        else if (deg < 157.5) { down = true; left = true; }
        else if (deg < 202.5) left = true;
        else if (deg < 247.5) { left = true; up = true; }
        else if (deg < 292.5) up = true;
        else { up = true; right = true; }
      }
      const nextDash = (dist >= deadR) && (dist >= (touchState.dash ? sprintOffR : sprintOnR));
      if (tJoyZone) tJoyZone.classList.toggle('sprinting', nextDash);
      if (up !== touchState.up || down !== touchState.down || left !== touchState.left ||
          right !== touchState.right || nextDash !== touchState.dash) {
        touchState.up = up;
        touchState.down = down;
        touchState.left = left;
        touchState.right = right;
        touchState.dash = nextDash;
        pushTouchPortsNow();
      }
    };

    if (tJoyZone) {
      tJoyZone.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        ensureAudio();
        if (joyPointerId !== null) return;
        joyPointerId = e.pointerId;
        try { tJoyZone.setPointerCapture(e.pointerId); } catch {}
        const zRect = tJoyZone.getBoundingClientRect();
        const aRect = tJoyAnchor ? tJoyAnchor.getBoundingClientRect() : null;
        const radius = (aRect && aRect.width > 20) ? (aRect.width * 0.5) : 56;
        joyOrigin = { cx: e.clientX, cy: e.clientY, radius };
        if (tJoyAnchor) {
          tJoyAnchor.style.left = `${(e.clientX - zRect.left).toFixed(1)}px`;
          tJoyAnchor.style.top = `${(e.clientY - zRect.top).toFixed(1)}px`;
        }
        tJoyZone.classList.add('active');
        updateJoyFromPoint(e.clientX, e.clientY);
      });
      tJoyZone.addEventListener('pointermove', (e) => {
        if (e.pointerId !== joyPointerId) return;
        e.preventDefault();
        updateJoyFromPoint(e.clientX, e.clientY);
      });
      const releaseJoy = (e) => {
        if (e.pointerId !== joyPointerId) return;
        joyPointerId = null;
        tJoyZone.classList.remove('active', 'sprinting');
        if (tJoyAnchor) {
          tJoyAnchor.style.left = '';
          tJoyAnchor.style.top = '';
        }
        if (tJoyKnob) tJoyKnob.style.transform = 'translate(-50%, -50%)';
        if (touchState.up || touchState.down || touchState.left || touchState.right || touchState.dash) {
          touchState.up = false;
          touchState.down = false;
          touchState.left = false;
          touchState.right = false;
          touchState.dash = false;
          pushTouchPortsNow();
        }
      };
      tJoyZone.addEventListener('pointerup', releaseJoy);
      tJoyZone.addEventListener('pointercancel', releaseJoy);
      tJoyZone.addEventListener('lostpointercapture', releaseJoy);
    }

    const syncAbcFromPointers = () => {
      let a = false, b = false, c = false, abcPressed = false;
      let visA = false, visB = false, visC = false;
      let pairAB = false, pairAC = false, pairBC = false;

      const markPair = (p1, p2) => {
        if ((p1 === 'a' && p2 === 'b') || (p1 === 'b' && p2 === 'a')) pairAB = true;
        else if ((p1 === 'a' && p2 === 'c') || (p1 === 'c' && p2 === 'a')) pairAC = true;
        else if ((p1 === 'b' && p2 === 'c') || (p1 === 'c' && p2 === 'b')) pairBC = true;
      };

      for (const entry of abcPointers.values()) {
        if (entry.mode === 'abc') {
          a = true; b = true; c = true; abcPressed = true;
          visA = true; visB = true; visC = true;
        } else if (entry.mode === 'pair') {
          if (entry.primary === 'a' || entry.second === 'a') { a = true; visA = true; }
          if (entry.primary === 'b' || entry.second === 'b') { b = true; visB = true; }
          if (entry.primary === 'c' || entry.second === 'c') { c = true; visC = true; }
          markPair(entry.primary, entry.second);
        } else {
          if (entry.primary === 'a' || entry.second === 'a') visA = true;
          if (entry.primary === 'b' || entry.second === 'b') visB = true;
          if (entry.primary === 'c' || entry.second === 'c') visC = true;
          if (entry.second) markPair(entry.primary, entry.second);
          if (entry.committed) {
            if (entry.primary === 'a' || entry.second === 'a') a = true;
            if (entry.primary === 'b' || entry.second === 'b') b = true;
            if (entry.primary === 'c' || entry.second === 'c') c = true;
          }
        }
      }

      for (const tail of abcPulseTails) {
        if (tail.btns.includes('a')) { a = true; visA = true; }
        if (tail.btns.includes('b')) { b = true; visB = true; }
        if (tail.btns.includes('c')) { c = true; visC = true; }
        if (tail.btns.length === 2) markPair(tail.btns[0], tail.btns[1]);
      }

      if (tBtnA) {
        tBtnA.classList.toggle('active', visA);
        tBtnA.classList.toggle('combo-active', pairAB || pairAC);
      }
      if (tBtnB) {
        tBtnB.classList.toggle('active', visB);
        tBtnB.classList.toggle('combo-active', pairAB || pairBC);
      }
      if (tBtnC) {
        tBtnC.classList.toggle('active', visC);
        tBtnC.classList.toggle('combo-active', pairAC || pairBC);
      }
      if (tBtnAbc) tBtnAbc.classList.toggle('active', abcPressed);
      if (tRouteAB) tRouteAB.classList.toggle('active', pairAB);
      if (tRouteAC) tRouteAC.classList.toggle('active', pairAC);
      if (tRouteBC) tRouteBC.classList.toggle('active', pairBC);
      if (tPillAB) tPillAB.classList.toggle('active', pairAB);
      if (tPillAC) tPillAC.classList.toggle('active', pairAC);
      if (tPillBC) tPillBC.classList.toggle('active', pairBC);

      if (a !== touchState.a || b !== touchState.b || c !== touchState.c) {
        touchState.a = a;
        touchState.b = b;
        touchState.c = c;
        pushTouchPortsNow();
      }
    };

    const getBtnCenters = () => {
      const box = (el) => {
        if (!el) return null;
        const r = el.getBoundingClientRect();
        if (r.width <= 0 || r.height <= 0) return null;
        return { x: r.left + r.width * 0.5, y: r.top + r.height * 0.5, r: Math.max(r.width, r.height) * 0.5, rect: r };
      };
      return {
        abc: box(tBtnAbc),
        a: box(tBtnA),
        b: box(tBtnB),
        c: box(tBtnC),
        ab: box(tPillAB),
        ac: box(tPillAC),
        bc: box(tPillBC),
      };
    };

    const hitAbcStart = (clientX, clientY) => {
      const m = getBtnCenters();
      if (m.abc) {
        const r = m.abc.rect;
        if (clientX >= r.left - 8 && clientX <= r.right + 8 && clientY >= r.top - 8 && clientY <= r.bottom + 8) {
          return { mode: 'abc', primary: 'abc', second: null };
        }
      }
      let bestBtn = null;
      let bestBtnDist = Infinity;
      for (const id of ['a', 'b', 'c']) {
        const info = m[id];
        if (!info) continue;
        const d = Math.hypot(clientX - info.x, clientY - info.y);
        if (d <= info.r * 1.18 && d < bestBtnDist) {
          bestBtnDist = d;
          bestBtn = id;
        }
      }
      if (bestBtn) {
        return { mode: 'btn', primary: bestBtn, second: null };
      }
      const pairDefs = [
        ['ab', 'a', 'b'],
        ['ac', 'a', 'c'],
        ['bc', 'b', 'c'],
      ];
      let bestPair = null;
      let bestPairDist = Infinity;
      for (const [pid, p1, p2] of pairDefs) {
        const pInfo = m[pid];
        if (!pInfo) continue;
        const d = Math.hypot(clientX - pInfo.x, clientY - pInfo.y);
        if (d <= 28 && d < bestPairDist) {
          bestPairDist = d;
          bestPair = [p1, p2];
        }
      }
      if (bestPair) {
        return { mode: 'pair', primary: bestPair[0], second: bestPair[1] };
      }
      return null;
    };

    const detectComboPartner = (entry, clientX, clientY) => {
      const m = getBtnCenters();
      const orig = m[entry.primary];
      if (!orig) return entry.second;
      const others = ['a', 'b', 'c'].filter((id) => id !== entry.primary);

      // 1. Direct proximity or bridge-corridor intersection toward a neighbor (max 2 buttons total)
      let bestTarget = null;
      let bestScore = -1;
      for (const other of others) {
        const dst = m[other];
        if (!dst) continue;
        const ex = dst.x - orig.x;
        const ey = dst.y - orig.y;
        const edgeLen = Math.hypot(ex, ey);
        if (edgeLen < 1) continue;
        const ux = ex / edgeLen;
        const uy = ey / edgeLen;

        const dToOther = Math.hypot(clientX - dst.x, clientY - dst.y);
        if (dToOther <= dst.r * 1.32) {
          const score = 2.0 - dToOther / (dst.r * 2);
          if (score > bestScore) { bestScore = score; bestTarget = other; }
          continue;
        }

        const vx = clientX - orig.x;
        const vy = clientY - orig.y;
        const proj = vx * ux + vy * uy;
        const perp = Math.abs(vx * (-uy) + vy * ux);
        const frac = proj / edgeLen;
        if (frac >= 0.25 && frac <= 1.25 && perp <= orig.r * 1.35) {
          const score = 1.0 + frac;
          if (score > bestScore) { bestScore = score; bestTarget = other; }
          continue;
        }

        // 2. Fast swipe direction from touch-down point
        const sdx = clientX - entry.startX;
        const sdy = clientY - entry.startY;
        const sDist = Math.hypot(sdx, sdy);
        if (sDist >= 11) {
          const dot = (sdx / sDist) * ux + (sdy / sDist) * uy;
          if (dot >= 0.78) {
            const score = 0.5 + dot * Math.min(1.0, sDist / 24);
            if (score > bestScore) { bestScore = score; bestTarget = other; }
          }
        }
      }

      if (bestTarget) return bestTarget;
      // Only unlock combo if pulled cleanly back onto primary center with tiny total offset
      const dBack = Math.hypot(clientX - orig.x, clientY - orig.y);
      if (dBack <= orig.r * 0.52) return null;
      return entry.second;
    };

    const spawnAbcPulseTail = (btns, durationMs) => {
      const tail = { btns, timer: null };
      abcPulseTails.add(tail);
      tail.timer = setTimeout(() => {
        abcPulseTails.delete(tail);
        syncAbcFromPointers();
      }, durationMs);
      syncAbcFromPointers();
    };

    if (tAbcPad) {
      tAbcPad.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        ensureAudio();
        const hit = hitAbcStart(e.clientX, e.clientY);
        if (!hit) return;
        try { tAbcPad.setPointerCapture(e.pointerId); } catch {}
        const now = performance.now();
        const entry = {
          mode: hit.mode,
          primary: hit.primary,
          second: hit.second,
          startX: e.clientX,
          startY: e.clientY,
          committed: hit.mode !== 'btn',
          comboAtMs: hit.mode === 'pair' ? now : 0,
          graceTimer: null,
          holdTimer: null,
        };
        if (hit.mode === 'btn') {
          entry.graceTimer = setTimeout(() => {
            entry.graceTimer = null;
            if (!abcPointers.has(e.pointerId)) return;
            entry.committed = true;
            syncAbcFromPointers();
          }, 32);
        }
        abcPointers.set(e.pointerId, entry);
        syncAbcFromPointers();
      });

      tAbcPad.addEventListener('pointermove', (e) => {
        const entry = abcPointers.get(e.pointerId);
        if (!entry || entry.mode !== 'btn') return;
        e.preventDefault();
        const nextSecond = detectComboPartner(entry, e.clientX, e.clientY);
        if (nextSecond !== entry.second) {
          entry.second = nextSecond;
          if (nextSecond) {
            if (entry.graceTimer) {
              clearTimeout(entry.graceTimer);
              entry.graceTimer = null;
            }
            entry.committed = true;
            if (!entry.comboAtMs) entry.comboAtMs = performance.now();
          }
          syncAbcFromPointers();
        }
      });

      const releaseAbc = (e) => {
        const entry = abcPointers.get(e.pointerId);
        if (!entry) return;
        if (entry.graceTimer) {
          clearTimeout(entry.graceTimer);
          entry.graceTimer = null;
        }
        abcPointers.delete(e.pointerId);

        if (entry.mode === 'btn' && !entry.committed) {
          spawnAbcPulseTail([entry.primary], 52);
          return;
        }
        if (entry.second || entry.mode === 'pair') {
          const heldMs = entry.comboAtMs ? (performance.now() - entry.comboAtMs) : 0;
          const remMs = Math.max(0, 78 - heldMs);
          if (remMs >= 16) {
            spawnAbcPulseTail([entry.primary, entry.second], Math.round(remMs));
            return;
          }
        }
        syncAbcFromPointers();
      };
      tAbcPad.addEventListener('pointerup', releaseAbc);
      tAbcPad.addEventListener('pointercancel', releaseAbc);
      tAbcPad.addEventListener('lostpointercapture', releaseAbc);
    }

    const bindSysTouchBtn = (el, key) => {
      if (!el) return;
      let activeId = null;
      el.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        ensureAudio();
        activeId = e.pointerId;
        try { el.setPointerCapture(e.pointerId); } catch {}
        el.classList.add('active');
        touchState[key] = true;
        pushTouchPortsNow();
      });
      const releaseSys = (e) => {
        if (e.pointerId !== activeId) return;
        activeId = null;
        el.classList.remove('active');
        if (touchState[key]) {
          touchState[key] = false;
          pushTouchPortsNow();
        }
      };
      el.addEventListener('pointerup', releaseSys);
      el.addEventListener('pointercancel', releaseSys);
      el.addEventListener('lostpointercapture', releaseSys);
    };
    bindSysTouchBtn(tBtnCoin, 'coin');
    bindSysTouchBtn(tBtnStart, 'start');

    window.addEventListener('blur', () => {
      resetTouchInputState();
      pushTouchPortsNow();
    });

    document.addEventListener('fullscreenchange', () => {
      presenter.invalidateSize();
      if (lastRgba) presenter.present(lastRgba, lastW, lastH);
      else if (typeof presenter.repaintLast === 'function') presenter.repaintLast();
    });

    // Menú -> Video
    filterButtons.forEach((b, idx) => {
      if (!b) return;
      b.onclick = () => {
        b.blur();
        prefs.filterMode = idx;
        saveJsonPrefs(PREFS_KEY, prefs);
        applyVideoSettings();
        syncUiState();
      };
    });
    btnVidCrtMask.onclick = () => {
      prefs.crtMask = !prefs.crtMask;
      saveJsonPrefs(PREFS_KEY, prefs);
      applyVideoSettings();
      syncUiState();
    };
    if (btnVidHdMode) {
      btnVidHdMode.onclick = () => {
        btnVidHdMode.blur();
        const nextHd = !prefs.hdMode;
        prefs.hdMode = nextHd;
        if (nextHd) {
          prefs.lowRes224p = false;
          if (worker) {
            worker.postMessage({
              type: 'set_visual_prefs',
              markerFix: false,
              lowRes224p: false,
            });
          }
        }
        if (nextHd && prefs.filterMode === 1) {
          prefs.filterMode = (detectedMobile || detectedSoftwareGpu) ? 2 : 4;
        }
        setLayerMask(nextHd ? (dbgLayers | 0x08) : (dbgLayers & ~0x08));
        saveJsonPrefs(PREFS_KEY, prefs);
        applyVideoSettings();
        syncUiState();
      };
    }
    btnVidHd704.onclick = () => setLayerMask(dbgLayers ^ 0x08);
    if (btnVidGpuVdp2) {
      btnVidGpuVdp2.onclick = () => {
        btnVidGpuVdp2.blur();
        if (!gpuVdpSupported) return;
        gpuVdpActive = !gpuVdpActive;
        gpuVdp1Active = gpuVdpActive && wantGpuVdp1 && gpuVdp1Supported;
        if (!gpuVdpActive && !bandPoolActive && cachedWasmBytesForBands && worker) {
          const bp = startBandPool(cachedWasmBytesForBands);
          if (bp.bandSab && bp.emuBandPorts.length > 0) {
            worker.postMessage({
              type: 'set_band_pool',
              bandSab: bp.bandSab,
              bandPorts: bp.emuBandPorts,
            }, bp.emuBandPorts);
          }
        }
        if (worker) {
          worker.postMessage({ type: 'set_gpu_vdp', enabled: gpuVdpActive, gpuVdp1: gpuVdp1Active });
        }
        stats.videoBackend = currentVideoBackendLabel();
        syncUiState();
      };
    }
    if (btnVidLowRes224) {
      btnVidLowRes224.onclick = () => {
        btnVidLowRes224.blur();
        prefs.lowRes224p = !prefs.lowRes224p;
        if (prefs.lowRes224p && prefs.hdMode) {
          prefs.hdMode = false;
          applyVideoSettings();
        }
        saveJsonPrefs(PREFS_KEY, prefs);
        if (worker) {
          worker.postMessage({
            type: 'set_visual_prefs',
            markerFix: false,
            lowRes224p: Boolean(prefs.lowRes224p),
          });
        }
        syncUiState();
      };
    }
    const setTargetFpsMode = (mode, btnEl) => {
      if (btnEl) btnEl.blur();
      prefs.fpsMode = (window.TwcPerfProfile && window.TwcPerfProfile.normalizeFpsMode)
        ? window.TwcPerfProfile.normalizeFpsMode(mode)
        : mode;
      lowPerfDismissed = false;
      saveJsonPrefs(PREFS_KEY, prefs);
      if (worker) worker.postMessage({ type: 'set_fps_mode', mode: prefs.fpsMode });
      syncUiState();
    };
    if (btnVidFpsAuto) btnVidFpsAuto.onclick = () => setTargetFpsMode('auto', btnVidFpsAuto);
    if (btnVidFps60) btnVidFps60.onclick = () => setTargetFpsMode('60', btnVidFps60);
    if (btnVidFps30) btnVidFps30.onclick = () => setTargetFpsMode('30', btnVidFps30);
    if (btnLowPerfAudioLite) {
      btnLowPerfAudioLite.onclick = () => {
        btnLowPerfAudioLite.blur();
        realScspAudio = !realScspAudio;
        prefs.realScsp = realScspAudio;
        saveJsonPrefs(PREFS_KEY, prefs);
        if (worker) worker.postMessage({ type: 'set_mode', realAudio: realScspAudio });
        syncUiState();
      };
    }
    if (btnLowPerfDismiss) {
      btnLowPerfDismiss.onclick = () => {
        btnLowPerfDismiss.blur();
        lowPerfDismissed = true;
        syncUiState();
      };
    }
    btnVidAspectFit.onclick = () => { prefs.aspect = 'fit'; saveJsonPrefs(PREFS_KEY, prefs); applyVideoSettings(); syncUiState(); };
    btnVidAspect43.onclick = () => { prefs.aspect = '4:3'; saveJsonPrefs(PREFS_KEY, prefs); applyVideoSettings(); syncUiState(); };
    btnVidAspectStretch.onclick = () => { prefs.aspect = 'stretch'; saveJsonPrefs(PREFS_KEY, prefs); applyVideoSettings(); syncUiState(); };
    btnVidFullscreen.onclick = () => toggleFullscreen();

    // Menú -> Audio
    btnAudMute.onclick = () => toggleMute();
    rngVolume.oninput = () => {
      prefs.volumePct = Math.max(0, Math.min(100, parseInt(rngVolume.value, 10) || 0));
      saveJsonPrefs(PREFS_KEY, prefs);
      audioEngine.setVolume(prefs.volumePct);
      syncUiState();
    };

    // Online Tab
    btnOnlineEdClassic.onclick = () => { btnOnlineEdClassic.blur(); selectEditionAndStart(false, false); };
    btnOnlineEdCopa.onclick = () => { btnOnlineEdCopa.blur(); selectEditionAndStart(true, false); };
    const setOnlineFormatCoop = (wantCoop) => {
      if (netSession.role === 'guest' || netSession.handshaking || netSession.inMatch) return;
      onlineCoopMode = Boolean(wantCoop);
      if (onlineCoopMode && !hardMode) {
        hardMode = true;
        prefs.hardMode = true;
        saveJsonPrefs(PREFS_KEY, prefs);
        pushSoloMatchRules();
      }
      if (netSession.connected && netSession.role === 'host') {
        try { rtcClient?.sendCtrl({ t: 'lobby_mode', copaMode, coopMode: onlineCoopMode, hardMode, matchRuleKey }); } catch {}
      }
      syncUiState();
    };
    if (btnOnlineFmtVs) btnOnlineFmtVs.onclick = () => { btnOnlineFmtVs.blur(); setOnlineFormatCoop(false); };
    if (btnOnlineFmtCoop) btnOnlineFmtCoop.onclick = () => { btnOnlineFmtCoop.blur(); setOnlineFormatCoop(true); };
    if (selOnlineRule) {
      selOnlineRule.onchange = () => {
        selOnlineRule.blur();
        if (netSession.role === 'guest' || netSession.handshaking || netSession.inMatch) return;
        matchRuleKey = String(selOnlineRule.value || 'default');
        prefs.matchRuleKey = matchRuleKey;
        saveJsonPrefs(PREFS_KEY, prefs);
        if (netSession.connected && netSession.role === 'host') {
          try { rtcClient?.sendCtrl({ t: 'lobby_mode', copaMode, coopMode: onlineCoopMode, hardMode, matchRuleKey }); } catch {}
        }
        syncUiState();
      };
    }
    if (btnOnlineHardMode) {
      btnOnlineHardMode.onclick = () => {
        btnOnlineHardMode.blur();
        if (netSession.role === 'guest' || netSession.handshaking || netSession.inMatch) return;
        hardMode = !hardMode;
        prefs.hardMode = hardMode;
        saveJsonPrefs(PREFS_KEY, prefs);
        pushSoloMatchRules();
        if (netSession.connected && netSession.role === 'host') {
          try { rtcClient?.sendCtrl({ t: 'lobby_mode', copaMode, coopMode: onlineCoopMode, hardMode, matchRuleKey }); } catch {}
        }
        syncUiState();
      };
    }
    selSigMode.onchange = () => { selSigMode.blur(); syncUiState(); };
    btnHostRoom.onclick = () => startHostRoom();
    btnJoinRoom.onclick = () => joinAsGuest();
    if (btnCopyInvite) {
      btnCopyInvite.onclick = async () => {
        btnCopyInvite.blur();
        const code = (inputRoomCode.value || '').trim().toUpperCase();
        if (!code) {
          setOnlineLogText('Primero pulsa Crear Sala para generar un código.');
          return;
        }
        const inviteUrl = `${window.location.origin}${window.location.pathname}?room=${encodeURIComponent(code)}`;
        try {
          await navigator.clipboard.writeText(inviteUrl);
          setOnlineLogText(`Enlace copiado (sala ${code}). Compártelo con tu rival.`);
        } catch {
          setOnlineLogText(`Comparte este enlace: ${inviteUrl}`);
        }
      };
    }
    btnStartOnlineMatch.onclick = () => startOnlineMatchFromHost();
    btnOnlineRestart.onclick = () => startOnlineMatchFromHost();
    btnLeaveOnline.onclick = () => leaveOnlineSession();
    btnApplyManualToken.onclick = async () => {
      if (!rtcClient) return;
      try {
        if (netSession.role === 'host') {
          await rtcClient.hostAcceptManualAnswer(txtManualIn.value);
        } else {
          await joinAsGuest();
        }
      } catch (err) {
        setOnlineLogText(`Invalid SDP token: ${err.message}`);
      }
    };

    // Controls Tab (P1 & P2 independent reset + Gamepad remapping & 2-pad toggle)
    btnResetP1.onclick = () => {
      p1KeyMap = { ...DEFAULT_P1_KEYMAP };
      listeningTarget = null;
      saveJsonPrefs(KEYMAP_P1_KEY, p1KeyMap);
      renderRemapGrids();
      syncUiState();
    };
    btnResetP2.onclick = () => {
      p2KeyMap = { ...DEFAULT_P2_KEYMAP };
      listeningTarget = null;
      saveJsonPrefs(KEYMAP_P2_KEY, p2KeyMap);
      renderRemapGrids();
      syncUiState();
    };
    if (btnPadTwoMode) {
      btnPadTwoMode.onclick = () => {
        padPrefs.twoPads = !padPrefs.twoPads;
        listeningPadTarget = null;
        padListenBaseline = null;
        saveJsonPrefs(PADPREFS_KEY, padPrefs);
        renderPadRemapGrids();
        syncUiState();
      };
    }
    if (btnPadSwap) {
      btnPadSwap.onclick = () => {
        padPrefs.swapPads = !padPrefs.swapPads;
        listeningPadTarget = null;
        padListenBaseline = null;
        saveJsonPrefs(PADPREFS_KEY, padPrefs);
        renderPadRemapGrids();
        syncUiState();
      };
    }
    if (btnResetPad1) {
      btnResetPad1.onclick = () => {
        p1PadMap = { ...DEFAULT_PADMAP_P1 };
        listeningPadTarget = null;
        padListenBaseline = null;
        saveJsonPrefs(PADMAP_P1_KEY, p1PadMap);
        renderPadRemapGrids();
        syncUiState();
      };
    }
    if (btnResetPad2) {
      btnResetPad2.onclick = () => {
        p2PadMap = { ...DEFAULT_PADMAP_P2 };
        listeningPadTarget = null;
        padListenBaseline = null;
        saveJsonPrefs(PADMAP_P2_KEY, p2PadMap);
        renderPadRemapGrids();
        syncUiState();
      };
    }
    window.addEventListener('gamepadconnected', () => { renderPadRemapGrids(); syncUiState(); });
    window.addEventListener('gamepaddisconnected', () => {
      listeningPadTarget = null;
      padListenBaseline = null;
      renderPadRemapGrids();
      syncUiState();
    });

    // Debug Drawer Controls
    document.getElementById('btnStep1').onclick = () => stepOneWorkerFrame();
    document.getElementById('btnQuickMatch').onclick = () => triggerQuickMatch();
    const btnForceWin = document.getElementById('btnForceWin');
    if (btnForceWin) btnForceWin.onclick = () => triggerForceWin();
    speedButtons.forEach((b, idx) => { b.onclick = () => setSpeedIndex(idx); });
    slotButtons.forEach((b, idx) => { b.onclick = () => selectSlot(idx); });
    document.getElementById('btnSave').onclick = () => doQuickSave(slotIdx);
    document.getElementById('btnLoad').onclick = () => doQuickLoad(slotIdx);
    document.getElementById('btnCoin2').onclick = () => pulsePort('c', ~0x02, 8);
    document.getElementById('btnStart2').onclick = () => pulsePort('c', ~0x20, 8);

    btnRec.onclick = () => toggleRecordReplay();
    btnPlayReplay.onclick = () => togglePlayReplay();
    document.getElementById('btnBench600').onclick = () => runBench600();

    btnVidUseGpu.onclick = () => {
      prefs.useGpu = !prefs.useGpu;
      saveJsonPrefs(PREFS_KEY, prefs);
      applyVideoSettings();
      syncUiState();
    };
    btnAudRealScsp.onclick = () => {
      realScspAudio = !realScspAudio;
      prefs.realScsp = realScspAudio;
      saveJsonPrefs(PREFS_KEY, prefs);
      if (worker) worker.postMessage({ type: 'set_mode', realAudio: realScspAudio });
      syncUiState();
    };

    document.getElementById('btnDownloadReplay').onclick = () => {
      const bytes = exportReplayBytes();
      if (!bytes) return;
      const blob = new Blob([bytes], { type: 'application/octet-stream' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `twc98_slot${slotIdx}.twcr`;
      a.click();
      URL.revokeObjectURL(url);
    };
    const fileInput = document.getElementById('inputUploadReplay');
    document.getElementById('btnUploadReplay').onclick = () => fileInput.click();
    fileInput.onchange = async () => {
      const f = fileInput.files && fileInput.files[0];
      if (!f) return;
      const buf = new Uint8Array(await f.arrayBuffer());
      importReplayBytes(buf);
      fileInput.value = '';
    };

    const syncLayersFromChk = () => {
      const m = (chkHideVdp1.checked ? 1 : 0) |
                (chkHideRbg0.checked ? 2 : 0) |
                (chkHideNbg.checked ? 4 : 0) |
                (chkHd704.checked ? 8 : 0);
      setLayerMask(m);
    };
    chkHideVdp1.onchange = syncLayersFromChk;
    chkHideRbg0.onchange = syncLayersFromChk;
    chkHideNbg.onchange = syncLayersFromChk;
    chkHd704.onchange = syncLayersFromChk;
  }

  window.__twc98Actions = {
    quickSave: () => doQuickSave(slotIdx),
    quickLoad: () => doQuickLoad(slotIdx),
    runBench600,
    pulsePort,
    toggleDebug: (force) => setDebugMode(force === undefined ? !debugMode : force),
    setSpeed: setSpeedIndex,
    stepOne: async () => {
      paused = true;
      stepOneWorkerFrame();
      return stats.canvasSha;
    },
    selectSlot,
    toggleRecord: toggleRecordReplay,
    togglePlayReplay,
    setLayerMask,
    quickMatch: triggerQuickMatch,
    exportReplayBytes,
    selectEditionAndStart,
    setMainTab,
    startHostRoom,
    joinAsGuest,
    startOnlineMatchFromHost,
    leaveOnlineSession,
  };

  async function boot() {
    bindUi();
    requestAnimationFrame(tick);

    try {
      const bundle = await window.TwcVaultLoader.fetchBundleManifest();
      if (!bundle) {
        bundleId = 'dev';
        bundleShortId = 'dev';
        accessSub.textContent = 'Local dev environment.';
        const loaded = await window.TwcVaultLoader.loadDevAssets((p) => {
          accessStatus.textContent = p.message || 'Loading...';
        });
        await initEmulatorFromLoadedAssets(loaded);
        return;
      }

      bundleId = String(bundle.id || '') || 'live';
      bundleShortId = bundleId.slice(0, 8) || 'live';
      const chunkCount = bundle.chunks.length;
      const totalEncMb = (bundle.chunks.reduce((s, c) => s + c.bytes, 0) / 1048576).toFixed(2);
      const isPrivateBundle = !(bundle.openAccess && bundle.accessKey);
      if (isPrivateBundle) {
        accessTitle.textContent = t('vaultPrivateTitle');
      }
      accessSub.textContent = devModeEnabled
        ? `AES-256-GCM (${chunkCount} chunks, ${totalEncMb} MiB).`
        : t('vaultChecking');

      const attemptUnlock = async (passcode, fromUserGesture = false) => {
        if (fromUserGesture) ensureAudio();
        btnUnlockVault.disabled = true;
        try {
          const loaded = await window.TwcVaultLoader.unlockAndLoadEncrypted(bundle, passcode, (p) => {
            if (p.phase === 'chunks') {
              const pct = Math.min(45, Math.round((p.index / Math.max(1, p.total)) * 45));
              const barPct = 5 + pct;
              accessBar.style.width = `${barPct}%`;
              if (devModeEnabled) {
                accessStatus.textContent = p.message || 'Decrypting...';
              } else {
                const isNet = p.fromCache === false;
                const label = isNet ? t('vaultDownloading') : t('vaultLoading');
                accessSub.textContent = label;
                accessStatus.textContent = `${label} (${barPct}%)`;
              }
            } else if (p.phase === 'decompress') {
              accessBar.style.width = '50%';
              if (devModeEnabled) {
                accessStatus.textContent = p.message || 'Decrypting...';
              } else {
                accessSub.textContent = t('vaultLoading');
                accessStatus.textContent = `${t('vaultLoading')} (50%)`;
              }
            } else if (devModeEnabled) {
              accessStatus.textContent = p.message || 'Decrypting...';
            } else {
              accessStatus.textContent = t('vaultLoading');
            }
          });
          await initEmulatorFromLoadedAssets(loaded);
        } catch (err) {
          btnUnlockVault.disabled = false;
          accessBar.style.width = '0%';
          accessFormWrap.style.display = 'flex';
          if (err.message === 'PASSCODE_INVALID' || err.message === 'PASSCODE_EMPTY') {
            accessStatus.textContent = t('vaultInvalidCode');
          } else {
            accessStatus.textContent = `Error: ${err.message}`;
          }
        }
      };

      btnUnlockVault.onclick = () => attemptUnlock(inputPasscode.value, true);
      inputPasscode.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') attemptUnlock(inputPasscode.value, true);
      });
      btnClearVaultCache.onclick = async () => {
        await window.TwcVaultLoader.clearVaultCache();
        accessStatus.textContent = t('vaultCacheCleared');
      };

      const hashKey = window.TwcVaultLoader.extractHashKey();
      if (bundle.openAccess && bundle.accessKey) {
        accessFormWrap.style.display = 'none';
        await attemptUnlock(bundle.accessKey, false);
      } else if (hashKey) {
        inputPasscode.value = hashKey;
        accessFormWrap.style.display = 'flex';
        await attemptUnlock(hashKey, false);
        stripSensitiveUrlParams();
      } else {
        accessFormWrap.style.display = 'flex';
        accessStatus.textContent = t('vaultEnterCode');
        inputPasscode.focus();
      }
    } catch (err) {
      stats.error = String(err && err.message ? err.message : err);
      stageStatusText.textContent = `ERROR: ${stats.error}`;
      accessStatus.textContent = `Error: ${stats.error}`;
    }
  }

  boot();
})();

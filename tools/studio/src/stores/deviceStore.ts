/**
 * BinaryKeyboard 设备状态管理
 */

import { defineStore } from "pinia";
import { ref, computed } from "vue";
import { hidService } from "@/services/HidService";
import { useMacroStore } from "@/stores/macroStore";
import {
  type DeviceCapabilities,
  type DeviceInfo,
  type DeviceStatus,
  type KeymapConfig,
  type RgbConfig,
  type FnKeyConfig,
  type KeyAction,
  type KeyTuningConfig,
  ActionSlot,
  getSlotKeys,
  OsMode,
  type OsModeConfig,
  KeyboardTypeInfo,
  createEmptyKeymap,
  createEmptyFnKeyConfig,
  createDefaultRgbConfig,
  createDefaultKeyTuning,
  MAX_LAYERS,
} from "@/types/protocol";

export const useDeviceStore = defineStore("device", () => {
  const EMPTY_CAPABILITIES: DeviceCapabilities = {
    multiLayer: false,
    layerKeyActions: false,
    rgb: false,
    rgbOverlay: false,
    fnKeys: false,
    osMode: false,
    macroActions: false,
    wheelClickAction: false,
    battery: false,
    logs: false,
    reset: false,
    explicitSave: false,
    wireless: false,
    iap: false,
    keyGestures: false,
  };

  // ========================================
  // 状态
  // ========================================

  /** HID 设备实例 */
  const device = ref<HIDDevice | null>(null);

  /** 设备信息 */
  const deviceInfo = ref<DeviceInfo | null>(null);

  /** 设备状态 */
  const deviceStatus = ref<DeviceStatus | null>(null);

  /** 按键映射配置 */
  const keymap = ref<KeymapConfig>(createEmptyKeymap());

  /** 原始按键映射 (用于比较变更) */
  const keymapOriginal = ref<KeymapConfig>(createEmptyKeymap());

  /** RGB 配置 */
  const rgbConfig = ref<RgbConfig>(createDefaultRgbConfig());

  /** FN 键配置 */
  const fnKeyConfig = ref<FnKeyConfig>(createEmptyFnKeyConfig());

  /** Win/Mac 系统模式 */
  const osModeConfig = ref<OsModeConfig>({ mode: OsMode.WIN });

  /** 当前编辑的层索引 */
  const currentEditLayer = ref(0);

  /** 当前编辑的动作槽位 (单击 / 双击 / 长按) */
  const currentActionSlot = ref<ActionSlot>(ActionSlot.CLICK);

  /** 按键判定参数 (长按阈值 / 双击窗口) */
  const keyTuning = ref<KeyTuningConfig>(createDefaultKeyTuning());

  /** 原始按键判定参数 (用于比较变更) */
  const keyTuningOriginal = ref<KeyTuningConfig>(createDefaultKeyTuning());

  /** 电池电压 (V, 如 4.12) */
  const batteryVoltage = ref(0);

  /** IAP 更新中 (抑制断连跳转) */
  const iapInProgress = ref(false);

  /** 加载状态 */
  const isLoading = ref(false);

  /** 错误信息 */
  const errorMessage = ref<string | null>(null);

  /** 实时轮询定时器 */
  let _pollTimer: ReturnType<typeof setInterval> | null = null;
  /** 轮询周期计数 (用于电压低频采样) */
  let _pollTick = 0;

  function cloneKeymapConfig(config: KeymapConfig): KeymapConfig {
    return JSON.parse(JSON.stringify(config)) as KeymapConfig;
  }

  function normalizeKeymapConfig(config: KeymapConfig): KeymapConfig {
    const normalized = cloneKeymapConfig(config);
    const info = deviceInfo.value;

    if (!info?.capabilities.multiLayer) {
      normalized.numLayers = 1;
      normalized.currentLayer = 0;
      normalized.defaultLayer = 0;
      return normalized;
    }

    const supportedLayers = Math.max(
      1,
      info.maxLayers ||
        KeyboardTypeInfo[info.keyboardType]?.layers ||
        normalized.numLayers ||
        1,
    );

    normalized.numLayers = Math.min(
      Math.max(normalized.numLayers || 1, 1),
      supportedLayers,
    );
    normalized.currentLayer = Math.min(
      normalized.currentLayer || 0,
      normalized.numLayers - 1,
    );
    normalized.defaultLayer = Math.min(
      normalized.defaultLayer || 0,
      normalized.numLayers - 1,
    );

    return normalized;
  }

  // ========================================
  // 计算属性
  // ========================================

  /** 是否已连接 */
  const isConnected = computed(() => device.value !== null && device.value.opened);

  /** 当前设备能力 */
  const capabilities = computed<DeviceCapabilities>(() => {
    return deviceInfo.value?.capabilities ?? EMPTY_CAPABILITIES;
  });

  const supportsMultiLayer = computed(() => capabilities.value.multiLayer);
  const supportsLayerKeyActions = computed(
    () => capabilities.value.layerKeyActions,
  );
  const supportsRgb = computed(() => capabilities.value.rgb);
  const supportsRgbOverlay = computed(() => capabilities.value.rgbOverlay);
  const supportsFnKeys = computed(() => capabilities.value.fnKeys);
  const supportsOsMode = computed(() => capabilities.value.osMode);
  const supportsMacroActions = computed(() => capabilities.value.macroActions);
  const supportsWheelClickAction = computed(
    () => capabilities.value.wheelClickAction,
  );
  /** 是否支持普通键双击 / 长按动作槽 */
  const supportsKeyGestures = computed(() => capabilities.value.keyGestures);
  const supportsBattery = computed(() => capabilities.value.battery);
  const supportsLogs = computed(() => capabilities.value.logs);
  const supportsFactoryReset = computed(() => capabilities.value.reset);
  const supportsExplicitSave = computed(() => capabilities.value.explicitSave);
  const supportsWireless = computed(() => capabilities.value.wireless);

  /** 键盘类型名称 */
  const keyboardTypeName = computed(() => {
    if (!deviceInfo.value) return "未知设备";
    return KeyboardTypeInfo[deviceInfo.value.keyboardType]?.name || "未知型号";
  });

  /** 实际可用键数 */
  const actualKeyCount = computed(() => {
    if (!deviceInfo.value) return 4;
    return deviceInfo.value.actualKeyCount;
  });

  /** 固件是否为本地 dev 构建 */
  const isDevFirmware = computed(() => {
    if (!deviceInfo.value) return false;
    const { versionMajor: maj, versionMinor: min, versionPatch: pat } = deviceInfo.value;
    return maj === 0 && min === 0 && pat === 0;
  });

  /** 固件版本字符串 (内部比较用: dev 或 x.y.z) */
  const firmwareVersion = computed(() => {
    if (!deviceInfo.value) return "0.0.0";
    if (isDevFirmware.value) return "dev";
    const { versionMajor: maj, versionMinor: min, versionPatch: pat } = deviceInfo.value;
    return `${maj}.${min}.${pat}`;
  });

  /** 固件版本显示文案 */
  const firmwareVersionLabel = computed(() => {
    if (!deviceInfo.value) return "v0.0.0";
    return isDevFirmware.value ? "dev" : `v${firmwareVersion.value}`;
  });

  /** 当前层的按键列表 (单击槽) */
  const currentLayerKeys = computed(() => {
    return (
      keymap.value.layers[currentEditLayer.value]?.keys.slice(
        0,
        actualKeyCount.value,
      ) || []
    );
  });

  /** 当前层 + 当前动作槽的按键列表 */
  const currentLayerSlotKeys = computed(() => {
    const layer = keymap.value.layers[currentEditLayer.value];
    if (!layer) return [];
    return getSlotKeys(layer, currentActionSlot.value).slice(
      0,
      actualKeyCount.value,
    );
  });

  /** 是否有未保存的更改 */
  const hasChanges = computed(() => {
    return (
      JSON.stringify(keymap.value) !== JSON.stringify(keymapOriginal.value) ||
      JSON.stringify(keyTuning.value) !==
        JSON.stringify(keyTuningOriginal.value)
    );
  });

  /** 按键判定参数是否有未保存的更改 */
  const keyTuningHasChanges = computed(() => {
    return (
      JSON.stringify(keyTuning.value) !==
      JSON.stringify(keyTuningOriginal.value)
    );
  });

  /** 设备信息列表 (用于 UI 显示) */
  const deviceInfoList = computed(() => {
    if (!deviceInfo.value) return [];
    return [
      { key: "芯片家族", value: deviceInfo.value.chipFamily },
      { key: "型号名称", value: keyboardTypeName.value },
      { key: "按键数量", value: `${actualKeyCount.value} 键` },
      { key: "固件版本", value: firmwareVersionLabel.value },
    ];
  });

  // ========================================
  // 方法
  // ========================================

  /** 连接设备 */
  async function connectDevice(hidDevice: HIDDevice): Promise<boolean> {
    const opened = await openDevice(hidDevice);
    if (!opened) {
      return false;
    }

    try {
      await initializeConnectedDevice();
      if (supportsMacroActions.value) {
        await useMacroStore().refreshOverview().catch(() => {});
      }
      return true;
    } catch {
      return false;
    }
  }

  function resetDeviceSession(): void {
    device.value = null;
    deviceInfo.value = null;
    deviceStatus.value = null;
    batteryVoltage.value = 0;
    keymap.value = createEmptyKeymap();
    keymapOriginal.value = createEmptyKeymap();
    rgbConfig.value = createDefaultRgbConfig();
    fnKeyConfig.value = createEmptyFnKeyConfig();
    osModeConfig.value = { mode: OsMode.WIN };
    currentEditLayer.value = 0;
    currentActionSlot.value = ActionSlot.CLICK;
    keyTuning.value = createDefaultKeyTuning();
    keyTuningOriginal.value = createDefaultKeyTuning();
    useMacroStore().reset();
  }

  async function openDevice(hidDevice: HIDDevice): Promise<boolean> {
    isLoading.value = true;
    errorMessage.value = null;
    const macroStore = useMacroStore();

    try {
      macroStore.reset();
      const success = await hidService.connect(hidDevice);
      if (!success) {
        throw new Error("无法打开设备");
      }

      device.value = hidService.getDevice() ?? hidDevice;
      return true;
    } catch (error) {
      errorMessage.value = error instanceof Error ? error.message : "连接失败";
      try {
        await hidService.disconnect();
      } catch {
        // ignore disconnect cleanup errors
      }
      resetDeviceSession();
      isLoading.value = false;
      return false;
    }
  }


  async function initializeConnectedDevice(): Promise<void> {
    if (!device.value) {
      isLoading.value = false;
      throw new Error("设备未连接");
    }

    errorMessage.value = null;

    try {
      await refreshDeviceInfo();
      await refreshKeymap();
      if (supportsRgb.value) {
        await refreshRgbConfig();
      } else {
        rgbConfig.value = createDefaultRgbConfig();
      }
      if (supportsFnKeys.value) {
        await refreshFnKeyConfig();
      } else {
        fnKeyConfig.value = createEmptyFnKeyConfig();
      }
      if (supportsOsMode.value) {
        await refreshOsMode();
      } else {
        osModeConfig.value = { mode: OsMode.WIN };
      }
      if (supportsKeyGestures.value) {
        await refreshKeyTuning();
      } else {
        keyTuning.value = createDefaultKeyTuning();
        keyTuningOriginal.value = createDefaultKeyTuning();
        currentActionSlot.value = ActionSlot.CLICK;
      }
    } catch (error) {
      errorMessage.value = error instanceof Error ? error.message : "连接失败";
      try {
        await hidService.disconnect();
      } catch {
        // ignore disconnect cleanup errors
      }
      resetDeviceSession();
      throw error instanceof Error ? error : new Error("连接失败");
    } finally {
      isLoading.value = false;
    }
  }

  async function refreshMacroOverview(): Promise<void> {
    if (!supportsMacroActions.value) {
      useMacroStore().reset();
      return;
    }
    await useMacroStore().refreshOverview();
  }

  /** 断开设备 */
  async function disconnectDevice(): Promise<void> {
    stopStatusPolling();
    await hidService.disconnect();
    resetDeviceSession();
    isLoading.value = false;
  }

  /** 刷新设备信息 */
  async function refreshDeviceInfo(): Promise<void> {
    deviceInfo.value = await hidService.getSysInfo();
    deviceStatus.value = await hidService.getSysStatus();
  }

  /** 刷新按键映射 */
  async function refreshKeymap(): Promise<void> {
    const config = normalizeKeymapConfig(await hidService.getFullKeymap());
    keymap.value = config;
    keymapOriginal.value = cloneKeymapConfig(config);
    currentEditLayer.value = config.currentLayer;
  }

  /** 刷新 RGB 配置 */
  async function refreshRgbConfig(): Promise<void> {
    if (!supportsRgb.value) {
      rgbConfig.value = createDefaultRgbConfig();
      return;
    }
    rgbConfig.value = await hidService.getRgbConfig();
  }

  /** 刷新 FN 键配置 */
  async function refreshFnKeyConfig(): Promise<void> {
    if (!supportsFnKeys.value) {
      fnKeyConfig.value = createEmptyFnKeyConfig();
      return;
    }
    fnKeyConfig.value = await hidService.getFnKeyConfig();
  }

  async function refreshOsMode(): Promise<void> {
    if (!supportsOsMode.value) {
      osModeConfig.value = { mode: OsMode.WIN };
      return;
    }
    osModeConfig.value = await hidService.getOsMode();
  }

  /** 保存按键映射到设备 */
  async function saveKeymap(): Promise<void> {
    isLoading.value = true;
    errorMessage.value = null;

    try {
      await hidService.setFullKeymap(keymap.value);
      if (supportsExplicitSave.value) {
        await hidService.saveConfig();
      }
      keymapOriginal.value = cloneKeymapConfig(keymap.value);
    } catch (error) {
      errorMessage.value = error instanceof Error ? error.message : "保存失败";
      throw error;
    } finally {
      isLoading.value = false;
    }
  }

  /** 保存 RGB 配置到设备 */
  async function saveRgbConfig(): Promise<void> {
    if (!supportsRgb.value) {
      throw new Error("当前设备不支持 RGB 配置");
    }
    isLoading.value = true;
    errorMessage.value = null;

    try {
      await hidService.setRgbConfig(rgbConfig.value);
      if (supportsExplicitSave.value) {
        await hidService.saveConfig();
      }
    } catch (error) {
      errorMessage.value = error instanceof Error ? error.message : "保存失败";
      throw error;
    } finally {
      isLoading.value = false;
    }
  }

  /** 保存 FN 键配置到设备 */
  async function saveFnKeyConfig(): Promise<void> {
    if (!supportsFnKeys.value) {
      throw new Error("当前设备不支持 FN 键配置");
    }
    isLoading.value = true;
    errorMessage.value = null;

    try {
      await hidService.setFnKeyConfig(fnKeyConfig.value);
      if (supportsExplicitSave.value) {
        await hidService.saveConfig();
      }
    } catch (error) {
      errorMessage.value = error instanceof Error ? error.message : "保存失败";
      throw error;
    } finally {
      isLoading.value = false;
    }
  }

  async function saveOsMode(mode: OsMode): Promise<void> {
    if (!supportsOsMode.value) {
      throw new Error("当前设备不支持 Win/Mac 模式切换");
    }
    isLoading.value = true;
    errorMessage.value = null;

    try {
      const next = { mode };
      await hidService.setOsMode(next);
      if (supportsExplicitSave.value) {
        await hidService.saveConfig();
      }
      osModeConfig.value = next;
    } catch (error) {
      errorMessage.value = error instanceof Error ? error.message : "系统模式保存失败";
      throw error;
    } finally {
      isLoading.value = false;
    }
  }

  /** 重置为出厂设置 */
  async function resetToFactory(): Promise<void> {
    if (!supportsFactoryReset.value) {
      throw new Error("当前设备不支持恢复出厂");
    }
    isLoading.value = true;
    errorMessage.value = null;

    try {
      const macroStore = useMacroStore();
      await hidService.resetConfig();
      macroStore.reset();
      await refreshDeviceInfo();
      await refreshKeymap();
      if (supportsRgb.value) {
        await refreshRgbConfig();
      }
      if (supportsFnKeys.value) {
        await refreshFnKeyConfig();
      }
      if (supportsOsMode.value) {
        await refreshOsMode();
      }
      if (supportsMacroActions.value) {
        await macroStore.refreshOverview().catch(() => {});
      }
    } catch (error) {
      errorMessage.value = error instanceof Error ? error.message : "重置失败";
      throw error;
    } finally {
      isLoading.value = false;
    }
  }

  /** 设置某个键的动作（可指定层与动作槽） */
  function setKeyAction(
    keyIndex: number,
    action: KeyAction,
    layerIndex?: number,
    slot?: ActionSlot,
  ): void {
    const layer = layerIndex ?? currentEditLayer.value;
    const targetSlot = slot ?? currentActionSlot.value;
    if (
      layer >= 0 &&
      layer < keymap.value.numLayers &&
      keyIndex >= 0 &&
      keyIndex < actualKeyCount.value
    ) {
      const keys = getSlotKeys(keymap.value.layers[layer], targetSlot);
      keys[keyIndex] = { ...action };
    }
  }

  /** 获取某个键的动作（可指定层与动作槽） */
  function getKeyAction(
    keyIndex: number,
    layerIndex?: number,
    slot?: ActionSlot,
  ): KeyAction | null {
    const layer = layerIndex ?? currentEditLayer.value;
    const targetSlot = slot ?? currentActionSlot.value;
    if (
      layer >= 0 &&
      layer < keymap.value.numLayers &&
      keyIndex >= 0 &&
      keyIndex < actualKeyCount.value
    ) {
      return getSlotKeys(keymap.value.layers[layer], targetSlot)[keyIndex] ?? null;
    }
    return null;
  }

  /** 切换编辑的动作槽位 */
  function setActionSlot(slot: ActionSlot): void {
    if (!supportsKeyGestures.value && slot !== ActionSlot.CLICK) {
      return;
    }
    currentActionSlot.value = slot;
  }

  /** 读取按键判定参数 */
  async function refreshKeyTuning(): Promise<void> {
    const tuning = await hidService.getKeyTuning();
    if (tuning) {
      keyTuning.value = { ...tuning };
      keyTuningOriginal.value = { ...tuning };
    } else {
      keyTuning.value = createDefaultKeyTuning();
      keyTuningOriginal.value = createDefaultKeyTuning();
    }
  }

  /** 保存按键判定参数（设备侧自动落盘） */
  async function saveKeyTuning(): Promise<boolean> {
    isLoading.value = true;
    errorMessage.value = null;
    try {
      const ok = await hidService.setKeyTuning(keyTuning.value);
      if (ok) {
        if (supportsExplicitSave.value) {
          await hidService.saveConfig();
        }
        keyTuningOriginal.value = { ...keyTuning.value };
      } else {
        errorMessage.value = "当前固件不支持按键判定参数设置";
      }
      return ok;
    } catch (err) {
      errorMessage.value =
        err instanceof Error ? err.message : "保存按键判定参数失败";
      return false;
    } finally {
      isLoading.value = false;
    }
  }

  /** 切换编辑层 */
  function setEditLayer(layerIndex: number): void {
    if (layerIndex >= 0 && layerIndex < keymap.value.numLayers) {
      currentEditLayer.value = layerIndex;
    }
  }

  /** 增加层数 */
  function addLayer(): boolean {
    const supportedLayers = Math.max(
      1,
      deviceInfo.value?.maxLayers ||
        KeyboardTypeInfo[deviceInfo.value?.keyboardType ?? 0]?.layers ||
        1,
    );

    if (keymap.value.numLayers < Math.min(MAX_LAYERS, supportedLayers)) {
      keymap.value.numLayers++;
      return true;
    }
    return false;
  }

  /** 减少层数 */
  function removeLayer(): boolean {
    if (keymap.value.numLayers > 1) {
      keymap.value.numLayers--;
      if (keymap.value.currentLayer >= keymap.value.numLayers) {
        keymap.value.currentLayer = keymap.value.numLayers - 1;
      }
      if (keymap.value.defaultLayer >= keymap.value.numLayers) {
        keymap.value.defaultLayer = 0;
      }
      if (currentEditLayer.value >= keymap.value.numLayers) {
        currentEditLayer.value = keymap.value.numLayers - 1;
      }
      return true;
    }
    return false;
  }

  /** 放弃更改 */
  function discardChanges(): void {
    keymap.value = cloneKeymapConfig(keymapOriginal.value);
    keyTuning.value = { ...keyTuningOriginal.value };
    currentEditLayer.value = keymap.value.currentLayer;
  }

  // ========================================
  // 实时轮询
  // ========================================

  /** 内部轮询: 每次取 SysStatus，并同步刷新电池电压 */
  async function _pollStatus(): Promise<void> {
    try {
      const status = await hidService.getSysStatus();
      deviceStatus.value = status;

      // 注释掉自动同步：让编辑层和当前层独立
      // 用户可以在设备使用层5的同时，在软件上编辑层2
      // if (status.currentLayer !== currentEditLayer.value) {
      //   currentEditLayer.value = status.currentLayer;
      // }

      _pollTick++;
      if (supportsBattery.value) {
        const bat = await hidService.getBattery();
        batteryVoltage.value = bat.voltage;
      }
    } catch {
      /* 轮询失败静默忽略, 下次重试 */
    }
  }

  /** 启动实时状态轮询 (2s 间隔) */
  function startStatusPolling(): void {
    stopStatusPolling();
    _pollTick = 0;
    if (supportsBattery.value) {
      hidService
        .getBattery()
        .then((bat) => {
          batteryVoltage.value = bat.voltage;
        })
        .catch(() => {});
    } else {
      batteryVoltage.value = 0;
    }
    _pollTimer = setInterval(_pollStatus, 2000);
  }

  /** 停止轮询 */
  function stopStatusPolling(): void {
    if (_pollTimer) {
      clearInterval(_pollTimer);
      _pollTimer = null;
    }
  }

  return {
    // 状态
    device,
    deviceInfo,
    deviceStatus,
    keymap,
    keymapOriginal,
    rgbConfig,
    fnKeyConfig,
    osModeConfig,
    currentEditLayer,
    currentActionSlot,
    keyTuning,
    keyTuningOriginal,
    batteryVoltage,
    iapInProgress,
    isLoading,
    errorMessage,

    // 计算属性
    isConnected,
    capabilities,
    supportsMultiLayer,
    supportsLayerKeyActions,
    supportsRgb,
    supportsRgbOverlay,
    supportsFnKeys,
    supportsOsMode,
    supportsMacroActions,
    supportsWheelClickAction,
    supportsKeyGestures,
    supportsBattery,
    supportsLogs,
    supportsFactoryReset,
    supportsExplicitSave,
    supportsWireless,
    keyboardTypeName,
    actualKeyCount,
    isDevFirmware,
    firmwareVersion,
    firmwareVersionLabel,
    currentLayerKeys,
    currentLayerSlotKeys,
    hasChanges,
    keyTuningHasChanges,
    deviceInfoList,

    // 方法
    connectDevice,
    openDevice,
    initializeConnectedDevice,
    disconnectDevice,
    refreshDeviceInfo,
    refreshKeymap,
    refreshRgbConfig,
    refreshFnKeyConfig,
    refreshOsMode,
    refreshMacroOverview,
    refreshKeyTuning,
    saveKeymap,
    saveKeyTuning,
    saveRgbConfig,
    saveFnKeyConfig,
    saveOsMode,
    resetToFactory,
    setKeyAction,
    getKeyAction,
    setActionSlot,
    setEditLayer,
    addLayer,
    removeLayer,
    discardChanges,
    startStatusPolling,
    stopStatusPolling,
  };
});

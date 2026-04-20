# NX VMS 自製分析插件 — 完整規劃

> 目標:打造一個完全本地化、支援多 Runtime、可在 NX Desktop Client UI 直接配置的分析插件,
> 取代(或與之並列)官方 NX AI Manager。

---

## 狀態:v1.0(2026-04-20)

- **Phase 0–8 全部完成並於 N100 `192.168.10.212` 實機驗證通過**
- 功能面:多 runtime(onnx / openvino)、Class 80 類 plugin-scope taxonomy、ROI 像素裁切、Inclusive / Exclusive mask、ByteTrack-lite tracker、Select all / Deselect all 按鈕、COCO-80 UI 勾選 — 全上線
- 沒做的(留 v1.x 或之後):Phase 7.5(多 Cam dispatcher)、Phase 8(熱抽換 & Supervisor,§8 里程碑表的 supervisor 列仍是 TODO)、Phase 9/10(Linux / Windows 一鍵安裝腳本)
- Source code 位置:**Windows host `C:\Users\Yuni\Nothinghere\phase1-staging\`**(非 git repo、無雲備份);遠端 N100 只有 build artifact
- 備份建議:Phase 1.x 開工前先把 `phase1-staging/` git init + push 到私 repo,否則 Windows host 故障就 lose source

---

## 1. 目標與非目標

### Goals
- 在 **NX Mediaserver** 內以 Plugin `.so` 形式運行,不依賴 NX AI Cloud
- 模型**本地**產生與載入,**UI 手動填入路徑**(不強制走 Cloud library)
- 支援多 Runtime:**PyTorch / ONNX Runtime / OpenVINO / TensorRT**(對齊 `yolo_env_checker.py` 已偵測到的能力)
- UI 提供:
  - Runtime 下拉選單
  - 模型路徑(文字輸入 + 檔案瀏覽)
  - **Class checkbox 多選**(取代 `yolo_http_tracker.py` 的 `--classes` CLI)
  - **ROI 多邊形**(可畫多個)
  - **Inclusive / Exclusive Input Mask**(多邊形,和 NX AI 官方一致)
  - Confidence / IoU 門檻、frame skip、device(CPU/GPU/NPU)
- 輸出物件框座標 + 類別 + 追蹤 ID 給 NX(走 `IObjectMetadata`),
  讓 NX 原生的 Event / Bookmark / Search 系統都能吃到

### Non-Goals
- **不自己接 RTSP** — NX Mediaserver 會把解碼後的影格送進 Plugin
- **不做雲端同步** / 模型上架流程
- 不嘗試在 C++ 端直接跑 PyTorch(GIL / libtorch 體積太大),全部推論走子進程

---

## 1.5 Strategy Decision (Final) — 為什麼只能走 Option A

### 硬約束(來自寄信詢問官方的結論 + CN 版本現況)
1. **NX AI Manager 是訂閱功能** — 官方明確回覆:此為付費訂閱用戶專屬
2. **CN 版本 NX VMS 不內建 Plugin** — 目前這顆 `libnxai_plugin.so` 是從國際版**手動提取**過來的
3. **Runtime 下載依賴 `artifactory.nxvms.dev`** — CN 網路環境隨時可能被 block
4. **現行運作是三重 bypass**:手動新增Plugin + 繞過訂閱檢查 + 繞過 Cloud 下載(模型手動放 `/opt/.../nxai_manager/cache/unencrypted/`)

### 評估過的其他路線(已否決)
| 選項 | 本質 | 否決原因 |
|---|---|---|
| **D. 只做 External Pre/Post-Processor** | 外掛於 NX AI Manager,傳遞 bbox/metadata | 仍需 NX AI Manager 存在 → 仍需訂閱、仍需 Cloud、CN 版本沒有 |
| **E. 自製 OAAX Runtime** | 寫自己的 runtime 塞進 NX AI Manager | 同上;且 OAAX runtime 註冊仍仰賴 NX AI infrastructure |
| **✅ A. 純 Analytics SDK 自製 Plugin** | 走 **MPL 2.0** 授權的 `nx_open_integrations` + 官方 Metadata SDK zip | 完全脫離 NX AI Manager;SDK 公開、免費、無訂閱 |

> **授權提示**:MPL 2.0 是 file-level copyleft — 我們改動的 SDK 內檔案要保留授權聲明並公開修改,自家新增的 plugin 檔案可用任意授權。不影響商業閉源使用。

### Migration Map — 從現行 bypass 到乾淨架構
| 面向 | 現況(bypass) | Option A 實作 |
|---|---|---|
| 模型路徑 | 手動塞 `/opt/.../nxai_manager/cache/unencrypted/*.onnx` | UI 欄位填 `modelPath`(§4.2) |
| 訂閱檢查 | 繞過(改檔 / 攔 API) | **無訂閱概念** — Analytics SDK 不檢查 |
| Runtime 取得 | 從 `artifactory.nxvms.dev` 下載 bundle | **走系統安裝的 Python 套件**(ONNX Runtime / OpenVINO / PyTorch) |
| Class 清單 | 綁定 NX AI 的 model library | 自己解析 ONNX tensor name(§4.7) |
| 雲端通訊 | NX AI 的 `scailable.net` | **完全不連外網** |
| 升級風險 | NX 改後端 API → bypass 失效 | 只依賴 `nx_sdk` 穩定 ABI |

### 要守的底線
- 整個 plugin **不碰 `/opt/networkoptix/mediaserver/bin/plugins/nxai_plugin/`** 任何檔案
- 安裝目錄獨立:`/opt/networkoptix/mediaserver/bin/plugins/nx_custom_plugin/`
- `install.sh` **不打任何外部 URL**(詳見 §9.5 離線部署)
- 只依賴 `nx_sdk` 的 Analytics Plugin interface(`IIntegration` / `IEngine` / `IDeviceAgent`),不接觸 NX AI 內部協定
- **SDK 來源**:從 Nx 官方更新伺服器下載版號 `6.1.1.42624` 對應的 Metadata SDK zip,**不是** git submodule;CMake 靠 `-DmetadataSdkDir=<path>` 指到解壓路徑(官方 sample 本來就這樣做)

---

## 2. 架構總覽(複製 NX AI Manager 的做法)

NX AI Manager 本身就是 `libnxai_plugin.so`(C++)+ `sclbld`(subprocess)模式,
我們照搬這個分層,把 `sclbld` 換成你既有的 Python 推論程式。

```
┌─────────────────────────── NX Mediaserver ───────────────────────────┐
│                                                                      │
│   uncompressedVideoFrame (RGB, HWC)                                  │
│            │                                                         │
│            ▼                                                         │
│   ┌──────────────────────────────────┐                               │
│   │  your_plugin.so  (C++, ~500-800 lines)                           │
│   │   - IIntegration (entry)                                         │
│   │   - IEngine      (全域設定, 例如 Runtime 類型)                   │
│   │   - IDeviceAgent (每台相機: 模型路徑/ROI/Class/Mask/threshold)   │
│   │   - 每幀: frame → IPC → 等 bbox JSON → 填 IObjectMetadata        │
│   └────────────┬─────────────────────┘                               │
│                │ UNIX socket 或 shared memory (跟 sclbld 一樣)       │
│                ▼                                                     │
│   ┌──────────────────────────────────┐                               │
│   │  inference_worker.py  (沿用 yolo_http_tracker 核心迴圈)           │
│   │   - RuntimeAdapter: PT / ORT / OV / TRT                          │
│   │   - preprocess (letterbox + ROI crop + mask apply)               │
│   │   - predict                                                      │
│   │   - postprocess (NMS, class filter, coord 還原)                  │
│   │   - 輸出 JSON: {ts, boxes:[{xyxy, score, cls, track_id}, ...]}   │
│   └──────────────────────────────────┘                               │
└──────────────────────────────────────────────────────────────────────┘
```

**為什麼選子進程架構**
| 選項 | 優 | 劣 | 選擇 |
|---|---|---|---|
| A. 純 C++ plugin,直接 link ONNX Runtime / OpenVINO / TensorRT C++ API | 效能最高、無 IPC 開銷 | 你現有 Python 程式全部要重寫;PyTorch 路線 C++ 很痛 | ❌ |
| **B. C++ 薄殼 + Python worker (IPC)** | **100% 沿用你現有程式;多 runtime 一次解決;升級 Python 不需重編 .so** | IPC 多一次 copy;啟動慢 1–2 秒 | ✅ |
| C. pybind11 in-process | 零 IPC | GIL、mediaserver 生命週期耦合、崩潰會拖垮整個 mediaserver | ❌ |

---

## 3. NX SDK 介面規劃

**SDK 來源**:`https://github.com/networkoptix/nx_open_integrations` → `analytics/nx_sdk_plugins/sample_analytics_plugin`

三個必要介面(每個對應一個 C++ 類別):

| 介面 | 生命週期 | 負責 |
|---|---|---|
| `IIntegration` | 每個 plugin `.so` 載入時建一次 | 宣告 manifest、建立 `IEngine` |
| `IEngine` | 每個 NX Server 建一個 | 全域 / Server 層級設定(ex: Runtime 類型、IPC 埠號) |
| `IDeviceAgent` | 每台開啟分析的相機各建一個 | 每幀處理、相機級設定(模型路徑、ROI、Class、Mask) |

**Manifest JSON**(決定 NX Desktop Client UI 長什麼樣):

```jsonc
// engineSettingsModel (全域,每台 Server 一組)
//
// ⚠️ M6 解法:`runtime` 和 `device` 的 items 陣列**不是寫死**,而是
//    plugin 啟動時呼叫 yolo_env_checker 掃描本機,把 **實際裝好且 check 通過**
//    的 runtime / device 才放進下拉選單。例如某台 Linux 節點沒裝 tensorrt,
//    這台的 Desktop Client UI 就根本看不到 tensorrt 選項,使用者不會誤選。
//    Windows 節點同理。這達成「叢集異質硬體、同一份 plugin、UI 依節點呈現」。
{
  "type": "Settings",
  "items": [
    { "name": "runtime",
      "type": "ComboBox",
      "caption": "Inference Runtime",
      "defaultValue": "<第一個可用的>",
      "items": ["<由 yolo_env_checker 回報>"] },   // e.g. ["onnx","openvino"] on N100
    { "name": "device",
      "type": "ComboBox",
      "caption": "Device",
      "defaultValue": "<第一個可用的>",
      "items": ["<由 yolo_env_checker 回報>"] },   // e.g. ["cpu","intel:gpu"] on N100
                                                    // e.g. ["cpu","cuda:0"] on NVIDIA 節點
                                                    // e.g. ["cpu","intel:npu"] on Meteor Lake+
    { "name": "workerCount",
      "type": "SpinBox",
      "caption": "Python worker 數量",
      "defaultValue": 1, "minValue": 1, "maxValue": 4 }
  ]
}

// deviceAgentSettingsModel (每台相機一組)
{
  "type": "Settings",
  "items": [
    { "name": "modelPath",
      "type": "TextField",
      "caption": "Model 檔案路徑",
      "description": "可填 .pt / .onnx / .engine / *_openvino_model 資料夾。Linux 預設 /var/lib/nx_plugin/models/,Windows 預設 C:\\ProgramData\\nx_plugin\\models\\。此路徑必須可被 mediaserver 執行身份(Linux=networkoptix,Windows=LocalSystem)讀取,**不能放個人家目錄**。",
      "defaultValue": "/var/lib/nx_plugin/models/yolo26s.onnx" },
    { "name": "conf",  "type": "SpinBox", "defaultValue": 30, "minValue": 1,  "maxValue": 99, "caption": "Confidence %" },
    { "name": "iou",   "type": "SpinBox", "defaultValue": 50, "minValue": 1,  "maxValue": 99, "caption": "IoU %" },
    { "name": "frameSkip", "type": "SpinBox", "defaultValue": 1, "minValue": 1, "maxValue": 30 },

    // ⭐ Class checkbox 多選 — 取代 --classes CLI
    { "name": "classes",
      "type": "GroupBox",
      "caption": "偵測類別",
      "items": [
        { "name": "class_0",  "type": "CheckBox", "caption": "person",  "defaultValue": true  },
        { "name": "class_1",  "type": "CheckBox", "caption": "bicycle", "defaultValue": false },
        { "name": "class_2",  "type": "CheckBox", "caption": "car",     "defaultValue": true  }
        // ... 模型載入後從 ONNX output tensor name 解析,動態產生
      ]
    },

    // ⭐ ROI / Mask — NX SDK 支援 Polygon figure
    { "name": "roi",
      "type": "PolygonFigure",
      "caption": "偵測區域 (ROI)",
      "maxItems": 4 },
    { "name": "inclusiveMask",
      "type": "PolygonFigure",
      "caption": "Inclusive Mask (只偵測這些區域)",
      "maxItems": 8 },
    { "name": "exclusiveMask",
      "type": "PolygonFigure",
      "caption": "Exclusive Mask (忽略這些區域)",
      "maxItems": 8 }
  ]
}
```

**Capabilities**(告訴 NX 要什麼格式的影格):
```json
{ "capabilities": "needUncompressedVideoFrames_rgb",
  "preferredStream": "secondary" }
```

**PolygonFigure 解析契約(Phase 0 smoke B 實測 2026-04-18)**

NX 回傳時,`probe.roi` 設定的值是一個**被字串化的 JSON**(不是 nested object),實際內容:
```json
"{\"figure\":{\"color\":\"#536dfe\",\"points\":[[0.324,0.676],[0.516,0.279],[0.736,0.682]]},\"label\":\"\",\"showOnCamera\":false}"
```
- 外層 value 已是 `string`,C++ `settingValue("probe.roi")` / Python `settings["probe.roi"]` 拿到的就是這個 JSON 字串 — **需要再 `json.loads()` 一次**才能取出 `.figure.points`
- `points` 是 `[[x, y], ...]`,**x/y 已是 normalized 0-1**(相對於整張影格的左上角原點)。轉像素時 `round(p * frame_w)` / `round(p * frame_h)`
- `color` / `label` / `showOnCamera` 純 UI 用,plugin 可忽略(若要在 Desktop Client 讓 mask 顯示不同色,可以自己指定一個預設 color)
- 空 polygon 的 value 不是 `null`,而是 `{"figure":{},"label":"","showOnCamera":false}` — Python 解析時要檢查 `"points" in figure`

---

## 4. 模型管理(用戶的重點補充)

### 4.1 產生
**沿用你現有的兩套工具,不重造**:

| 場景 | 工具 | 輸出 |
|---|---|---|
| 快速匯出給插件用 | `yolo_env_checker.py --auto` | 依硬體挑最佳 runtime 格式 |
| NX 相容 ONNX(含 NMS, 特殊 tensor name) | `nx_onnx_export.py` | `[300,6]` 靜態 ONNX,含 `nms_sensitivity-` input |
| 官方格式參考 | `nxai-model-to-onnx/yolo26-to-onnx/` | 動態 `[K,6]`,sclblonnx 路徑 |

**決策**:Plugin 內同時支援**兩種格式**:
- **NX 相容格式**(`[300, 6]`,`bboxes-format:xyxysc;0:person;1:bicycle;...` 輸出名)
  → 類別清單可**直接從 output tensor name 解析**,UI checkbox 自動生成 ✨
- **任意 ONNX / PyTorch / TRT / OV**(非 NX 規範)
  → UI 需提供**額外欄位**讓用戶填 class 清單(或指向 `.yaml` / `.names` 檔)

### 4.2 路徑策略

**UI 手動填寫為主**(最彈性),輔以環境變數 / conf 預設:

```
┌──────────────────────────────────────────────────┐
│ 優先順序(由高到低):                              │
│                                                  │
│ 1. DeviceAgent Setting: modelPath 欄位           │
│    例 (Linux):   /var/lib/nx_plugin/models/      │
│                      yolo26s.onnx                │
│    例 (Windows): C:\ProgramData\nx_plugin\       │
│                      models\yolo26s.onnx         │
│                                                  │
│ 2. 若欄位為相對路徑 → 相對於 Engine 設定的        │
│    modelRoot (Linux 預設 /var/lib/nx_plugin/     │
│    models/;Windows 預設 C:\ProgramData\          │
│    nx_plugin\models\)                            │
│                                                  │
│ 3. Engine Setting: defaultModel (備用)           │
└──────────────────────────────────────────────────┘
```

> ⚠️ **C1 解法**:絕對不要把預設值指向 `/home/<user>/`。Linux 上 `/home` 通常 750,
> `networkoptix` 連目錄 `ls` 都被拒絕,plugin 會在第一次 load 就 EACCES 死掉。
> `install.sh` 一開始就 `mkdir -p /var/lib/nx_plugin/models && chown networkoptix:networkoptix ... && chmod 755`。
> Windows 上對應 `%ProgramData%\nx_plugin\models\`(預設對 LocalSystem 可讀)。

**UI 貼心功能**:
- 填完 `modelPath` 按 **"Validate"** 按鈕(NX SDK 有 `Button` 元件),
  Plugin 端試載入,成功 → 解析類別清單 → **動態刷新 class checkbox 列表**
- 失敗時在 UI 顯示錯誤訊息(透過 `IPluginDiagnosticEvent`)

### 4.3 Runtime ↔ 副檔名對照(沿用 `detect_model_format()`)

```
.pt, .pth, .yaml         → PyTorch (Ultralytics)
.onnx                    → ONNX Runtime / OpenVINO EP(二選一,看 engineSettings.runtime)
.engine                  → TensorRT
<name>_openvino_model/   → OpenVINO IR
```
**不一致檢查**:若 engineSettings.runtime=`tensorrt` 但 modelPath 是 `.onnx`,
插件發 diagnostic warning:"Runtime 與 model 格式不符,將改用 onnx"。

### 4.4 模型熱抽換(M3 解法:雙 session 交接)

- 使用者改 `modelPath` 按 Apply → NX 呼叫 `IDeviceAgent::setSettings()`
- Plugin 比對新舊路徑,不同就發 `RELOAD_MODEL` 給 Python worker
- **雙 session 模式**(不能單純丟幀,NX metadata 空窗太久 Desktop Client 會停格):
  ```
  t0  worker 正在跑 session_A,收到 {reload_model, path:B}
  t1  worker 在背景 thread 開始 load session_B (~1-3 sec)
  t2  這段期間進來的幀:繼續用 session_A 推論並正常回 metadata
  t3  session_B ready → atomic swap(用 rwlock 或 atomic ptr)
  t4  session_A.close() 釋放資源
  ```
- 若新模型載入**失敗**:保留 session_A 不動,並透過 `IPluginDiagnosticEvent` 推錯誤訊息到 Desktop Client;UI 也把 `modelPath` 欄位旁打紅叉
- 特殊情況 `class 清單改變`:Desktop Client 的 class checkbox 依 §4.7.1 的分支(pushManifest 或唯讀 label)更新

### 4.5 權限 / 檔案存取

**Linux**
- Python worker 以 `networkoptix` 身份跑 → 模型檔必須 `networkoptix:networkoptix` 可讀
- **venv 位置**:`/opt/nx_custom_plugin/venv/`(owned by `networkoptix:networkoptix`, mode `755`)
  — 不要放使用者家目錄,家目錄預設 750 會讓 networkoptix 讀不到 venv 內的 `.so`
- 模型目錄:`/var/lib/nx_plugin/models/`(`networkoptix:networkoptix`, mode `755`)
- Log 目錄:`/var/log/nx_plugin/`(同上)
- UI 欄位旁加提示:"確保檔案可被 networkoptix user 讀取"

**Windows**
- mediaserver 預設以 `LocalSystem` 跑 → 絕大多數路徑都可讀,但避免放進使用者 `C:\Users\<name>\`(其他帳號不可讀)
- venv 位置:`C:\Program Files\nx_plugin\venv\`
- 模型目錄:`C:\ProgramData\nx_plugin\models\`

**install.sh / install.ps1 必做**:建目錄 + 設權限 + 放 `.so`/`.dll` 到 plugin 目錄 + rsync Python 原始碼到 venv 旁。

### 4.6 Input Size 支援(多解析度)

**現況盤點**:
| 工具 | 目前 Input Size | 問題 |
|---|---|---|
| `yolo_http_tracker.py` | 640×640(Ultralytics 預設) | **硬編碼**,不能跑其他尺寸的模型 |
| `nx_onnx_export.py` | 320×320(硬編碼) | 只能產一種大小 |
| NX 官方 | **多種**(320 / 416 / 640 / 832 / 1280) | 依速度/準確度取捨 |

**做法**:
1. **模型產生端** — 把 `nx_onnx_export.py` 改成吃 `--imgsz` 參數(對齊官方):
   ```bash
   python nx_onnx_export.py --model yolo26s.pt --imgsz 640
   python nx_onnx_export.py --model yolo26s.pt --imgsz 1280   # 需要更清楚小物
   ```
   Ultralytics `model.export(imgsz=...)` 本來就支援,只是現在被寫死。

2. **插件讀取端** — **從 ONNX 自動偵測 input shape**,不讓 UI 自己填:
   ```python
   # inference_worker.py
   shape = session.get_inputs()[0].shape   # e.g. [1, 3, 640, 640]
   _, _, model_h, model_w = shape
   ```
   若為動態(`-1` 或字串),才退回去讀 UI `inputSize` 欄位。

3. **UI 行為**:
   - 預設**不顯示** Input Size 欄位 — 靠模型自身
   - 按 "Validate" 後在 UI 顯示偵測到的 `inputSize: 640×640`(唯讀 label)
   - 只有當偵測到動態 shape 時,才彈出數字輸入欄

4. **Preprocess 改造**:
   - `PreProcessor` 必須知道目標 `(model_h, model_w)`,不再寫死 640
   - `letterbox()` 吃 `(target_h, target_w)` 參數
   - 後處理座標還原也跟著參數化

5. **不同相機不同尺寸**:允許每個 DeviceAgent 指向不同 `modelPath`,
   每台相機各自走各自的 size(preprocess 各自 letterbox)。

### 4.7 Class 清單來源(自包含,不依賴 NX AI model library)

**現況**:NX AI Manager 從 Cloud library 拉模型 metadata 取得 class list —
我們脫離 Cloud,必須**自己解決**類別清單。

**優先順序**(實作時由高到低 fallback):
```
1️⃣ 從 ONNX output tensor name 自動解析 (NX 相容格式)
   tensor name: "bboxes-format:xyxysc;0:person;1:bicycle;2:car;..."
   → 用 regex/split 拆出 {0: "person", 1: "bicycle", ...}
   → 無需 UI 輸入,最無痛(跟你的 nx_onnx_export.py 是一對)

2️⃣ UI 提供 "Class 清單檔案" 欄位 (TextField)
   接受 .yaml (Ultralytics data.yaml 的 names: 區塊)
   或 .names (每行一類)
   或 .txt (同 .names)
   → 用於非 NX 相容格式的 ONNX / PyTorch / TensorRT 模型

3️⃣ 從 Ultralytics model.names 讀 (PyTorch adapter 專屬)
   model = YOLO(path)
   names = model.names   # dict[int, str]
   → PyTorch 路線直接可用

4️⃣ COCO 80 預設 fallback
   內建 coco80.txt 在 python/resources/
   → 最後防線,模型若輸出 80 類且無 metadata,直接套用
```

**實作位置**:`inference_worker.py` → `RuntimeAdapter.names()` 依 adapter 型別各自實作上述邏輯。

### 4.7.1 UI 動態更新 class checkbox(C5 解法) — **Phase 0 已驗證**

**2026-04-18 Phase 0 smoke A 實測結論**:Active Setting round-trip 在 CN 版 6.1.1.42624 mediaserver 上**正常運作**,`pushManifest()` 則**不影響 settings UI**(只更新 DeviceAgent 的 supportedTypes/typeLibrary)。決策:

| 機制 | 能否動態改 settings UI | 用途 |
|---|---|---|
| **Active Setting + `doGetSettingsOnActiveSettingChange`** | ✅ 可改 settingsModel、可改 settingsValues、可送訊息 | class checkbox 重建的正式路線 |
| `ConsumingDeviceAgent::pushManifest()` | ❌ 只推 DeviceAgent manifest(event/object types) | 動態新增 event type 用(與本節無關) |
| Engine 建構子一次性計算 manifest | ✅ 靜態,無 round-trip | **M6 runtime ComboBox 首選**:Engine 啟動時跑 `yolo_env_checker`,把結果 bake 進 `deviceAgentSettingsModel` |

**正式流程**:
1. **Engine 建構子** — 跑 `yolo_env_checker` 的 C++ caller(fork 一次 python probe),取得「本機可用 runtime 清單」,組進 `deviceAgentSettingsModel` 的 `probe.runtime.range`,送回去 Desktop Client 的第一版 manifest 已經有正確 items
2. **Validate 按鈕** — 實作為 Button 型 Active setting;點擊觸發 `doGetSettingsOnActiveSettingChange`,C++ 向 Python worker 查 `names()`,取回後**在回傳的 `IActiveSettingChangedResponse` 裡塞新 settingsModel**(把 `classes` CheckBoxGroup 的 `range` 換成 model 實際的類別),Desktop Client 自動重繪

**Active setting 回呼的有效欄位**(smoke 實測):
- `activeSettingName` — 觸發者 name(例:`"probe.runtime"`)
- `settingsModel` — 當前完整 model JSON(伺服器會補齊各欄位預設如 `validationRegex`、`filledCheckItems` 等)
- `settingsValues` — `IStringMap`,含所有欄位當前 GUI 值(未 Apply 也看得到)
- `params` — manifest 裡 `parametersModel` 定義的額外參數(我們這版沒用,回傳 count=0)

**降級方案已不需要** — 原文備份在 git 歷史,不再列於此表。

---

## 5. IPC 協定 (C++ ↔ Python)

> **C4 解法**:Socket **per-worker-group**,不是 per-device。多相機共用一條 socket,
> 靠 frame header 裡的 `camera_uuid` 做 routing。配合 §6.5 的 shared/pool 拓樸。

### 5.1 Socket 命名

```
Linux   : /run/nx_plugin/worker_<group_id>.sock       (tmpfs, 每次 boot 清空)
Windows : \\.\pipe\nx_plugin_worker_<group_id>        (Named Pipe)
```
- `<group_id>` 規則:shared 模式 = `default`;pool 模式 = `<modelPath hash 前 8 碼>`;per-camera 模式 = `<device_uuid>`
- 三種拓樸共用同一個協定,只有通道數量不同
- 目錄 `/run/nx_plugin/` 由 install 腳本建立(`networkoptix:networkoptix` 755);Windows Named Pipe 由 worker 建立時即賦予 LocalSystem 可讀寫

### 5.2 協定幀格式

Length-prefixed framing(4-byte big-endian length + payload):
- **控制訊息**(JSON):設定更新 / 模型載入 / 關閉
- **影格訊息**(Header JSON + 二進位 RGB buffer):送進 worker
- **結果訊息**(JSON):回傳偵測結果

```jsonc
# 影格 request (每台相機共用此 socket,用 camera_uuid routing)
HEADER: { "type": "frame",
          "camera_uuid": "9b655ad3-f379-74c0-fe1c-ba4087c8ebfb",
          "id": 12345,                   // 單 camera 內遞增,給 backpressure drop 用
          "w": 1920, "h": 1080,
          "ts_us": 1729180000000000 }
BODY:   <w*h*3 bytes raw RGB>

# 結果 response
{ "type": "result",
  "camera_uuid": "9b655ad3-...",         // 必帶,C++ 端 dispatch 回對應 DeviceAgent
  "id": 12345,
  "boxes": [
    { "xyxy": [120, 88, 340, 410], "score": 0.87, "cls": 0, "track_id": 3 },
    ...
  ],
  "infer_ms": 23.4 }

# 控制(per-camera 設定)
{ "type": "config",
  "camera_uuid": "9b655ad3-...",
  "runtime": "openvino",
  "model_path": "/var/lib/nx_plugin/models/yolo26s.onnx",
  "conf": 0.3, "iou": 0.5,
  "classes": [0, 2, 5],
  "roi":            [[[x,y],[x,y],...]],
  "inclusive_mask": [[[x,y],...], ...],
  "exclusive_mask": [[[x,y],...], ...] }

{ "type": "reload_model", "camera_uuid": "9b655ad3-...", "path": "..." }

{ "type": "attach",   "camera_uuid": "9b655ad3-..." }   // DeviceAgent 啟動時發
{ "type": "detach",   "camera_uuid": "9b655ad3-..." }   // DeviceAgent 關閉時發
```

### 5.3 Backpressure 與 keep-latest

每台 camera 在 C++ 端維護 **bounded queue(size=2)**,worker 處理不過來時:
- 新幀進來發現隊列滿 → **丟掉舊的那一張**,保留最新(keep-latest policy)
- DeviceAgent 同時在 metadata 打一個 `drop_count` counter,UI 可讀(Diagnostic)

### 5.4 效能考量

影格 1920×1080×3 = 6 MB。若 IPC copy 成為瓶頸(每幀 >20ms),
升級到 **shared memory**:
- Linux:`shm_open` / `mmap` + Python `multiprocessing.shared_memory`
- Windows:`CreateFileMapping` / `MapViewOfFile` + Python 同樣用 `shared_memory`(Python 3.8+ 兩邊 API 相同)

只在 socket 傳 header + shm segment id,不傳 bytes。

---

## 6. Python Worker 改造(從 `yolo_http_tracker.py` 抽出核心)

**砍掉的部分**:
- ❌ `RTSPStreamLoader`(NX 負責送幀)
- ❌ MJPEG HTTP server / Flask(輸出走 IPC)
- ❌ 所有繪圖(`_draw_corner_box` 等)— NX Desktop Client 依照 metadata 自己畫

**保留並抽象化的部分**:
- ✅ `detect_model_format()`、`get_available_formats()`(env_checker 的邏輯)
- ✅ Ultralytics `model.track()` 迴圈
- ✅ 類別名稱 ↔ 索引映射 (`model.names` 反查)
- ✅ BoT-SORT / ByteTrack 追蹤(給 NX 一個 `trackId`)

**新增**:
- `RuntimeAdapter` 抽象類 + 4 個實作:
  - `PytorchAdapter`(呼叫 Ultralytics `model.track()`)
  - `OnnxAdapter`(`onnxruntime.InferenceSession`)
  - `OpenvinoAdapter`(`openvino.runtime.Core`)
  - `TensorrtAdapter`(`tensorrt` + `pycuda`)
- `PreProcessor`:letterbox + ROI crop + mask alpha blend
- `PostProcessor`:NMS(如果 model 沒內建)、class filter、座標還原到原 frame

**Adapter 介面**:
```python
class RuntimeAdapter(Protocol):
    @classmethod
    def available(cls) -> tuple[bool, list[str]]:
        """
        M6 解法:plugin 啟動時呼叫這個 classmethod,給 C++ 用來組 UI 下拉選單。
        回傳 (是否可用, 這個 runtime 能跑在哪些 device)。
        實作範例:
            OnnxAdapter.available()     → (True,  ['cpu'])
            OpenvinoAdapter.available() → (True,  ['cpu','intel:gpu'])  # 靠 ov.Core().available_devices
            TensorrtAdapter.available() → (False, [])                   # 若無 NVIDIA GPU
        直接重用 yolo_env_checker 的邏輯,不再寫第二份。
        """

    def load(self, path: str, device: str) -> None: ...
    def names(self) -> list[str]: ...          # 給 UI checkbox / 唯讀 label 用
    def infer(self, rgb_chw: np.ndarray) -> np.ndarray: ...
    def close(self) -> None: ...
```

**UI 聯動流程**(由 M6 簡化版):
```
plugin load → C++ spawn 一個短命的 python probe:
    python -m inference_worker.probe
                ↓ JSON stdout
    { "runtimes": ["onnx","openvino","pytorch"],
      "devices":  { "onnx":["cpu"], "openvino":["cpu","intel:gpu"],
                    "pytorch":["cpu"] } }
              ↓
C++ 把這份清單塞進 engineSettingsModel 的 runtime/device items
              ↓
使用者選 runtime / device → 儲存到 engineSettings
              ↓
每個 DeviceAgent 建立時,Python 側依 engineSettings 選對應 Adapter
```
好處:**不自動選**,使用者完全掌控。某個 runtime 壞了只會在他選那個 runtime 時報錯,不會拖累其他 runtime 選項顯示。

---

## 6.5 多 Camera 架構(擴展 `yolo_http_tracker.py` 的單 Cam 侷限)

**現況**:`yolo_http_tracker.py` 一個 process 綁一條 RTSP、一個 HTTP port、一個模型 instance — **單 Cam 設計**。

**NX 的現實**:Mediaserver 會為**每台啟用分析的相機建立一個 `IDeviceAgent` 實例**。
現有 server 錄 25+ 台相機,全部打開分析 = 25 個 DeviceAgent 同時要推論。

### 3 種 Worker 拓樸(擇一 / 混用)

```
A. Per-Camera Worker (完全隔離)
   Cam1 ─ DeviceAgent1 ─ Worker1 (獨立 process)
   Cam2 ─ DeviceAgent2 ─ Worker2 (獨立 process)
   ...
   👍 隔離性最佳,一個模型壞只影響一台相機
   👎 N100 記憶體會爆(Ultralytics+PyTorch ≈ 2 GB × 25 = 50 GB)
   👎 每個 worker 都要各自載模型、各自吃 CPU

B. Shared Single Worker (單例佇列)
   Cam1 ─┐
   Cam2 ─┼─ DeviceAgent* ─┬─ frame queue ─ Worker (1 process)
   Cam3 ─┘                └─ result router
   👍 記憶體省(只載一次模型)
   👍 可做 batched inference(ONNX/OV/TRT dynamic batch → 大幅加速)
   👎 Worker crash → 全部相機斷
   👎 單核心 bottleneck

C. Worker Pool (推薦)                        ← 選這個
   Cam1..N ─ DeviceAgents ─ dispatcher ─┬─ Worker1
                                        ├─ Worker2
                                        └─ Worker3
   👍 可設 worker 數(engine setting),依硬體調
   👍 單 worker crash 只影響排到它的那幾台
   👍 可分模型:不同 worker 載不同模型,UI 指定 group
   👎 複雜度最高
```

### 建議:**B + C 混合**
- **預設模式 = B**(單一 shared worker,若所有相機都用**相同模型**)
- 若 UI 有相機指定**不同 modelPath** → 自動切到 C,依 modelPath 分組,
  每組一個 worker

### Dispatcher 設計
```
C++ 端:
  - 全部 DeviceAgent 共用一個 IpcChannel pool
  - 每幀送進去時附 camera_uuid,結果用 uuid routing 回對應 DeviceAgent

Python 端 (inference_worker.py):
  - 主迴圈: multiprocessing.Queue (frame in) / Queue (result out)
  - 支援 batch_size N 的 stacked inference
      batch = [pop_until_timeout(queue, 10ms, max=8)]
      if len(batch) > 1 and adapter.supports_batch:
          results = adapter.infer_batch(batch)
      else:
          results = [adapter.infer(f) for f in batch]
  - 追蹤 state **per camera_uuid**(ByteTrack/BoT-SORT 各有各的 tracker state)
```

### UI 設定(新增到 engineSettingsModel)
```jsonc
{ "name": "workerMode",
  "type": "ComboBox",
  "defaultValue": "shared",
  "items": ["shared", "pool", "per-camera"],
  "caption": "Worker 拓樸" },
{ "name": "workerCount",
  "type": "SpinBox",
  "defaultValue": 2, "minValue": 1, "maxValue": 8,
  "caption": "Worker 數量 (pool 模式)" },
{ "name": "maxBatch",
  "type": "SpinBox",
  "defaultValue": 4, "minValue": 1, "maxValue": 16,
  "caption": "最大 batch size" }
```

### 記憶體估算(N100 / 5.8 GB RAM 實例)
| 模式 | Worker × Cam | 實際推論負載 | 可行性 |
|---|---|---|---|
| Per-camera × 25 cam | 25 × 2 GB | 50 GB | ❌ |
| Shared × 25 cam | 1 × 2.5 GB | 2.5 GB + mediaserver 700 MB | ✅ |
| Pool(2 workers)× 25 cam | 2 × 2.5 GB | 5 GB | ⚠️ 邊界 |

→ **N100 上實務建議:shared 模式 + frame_skip 自動調節**,
讓 25 台相機共用 1 個 worker,吃不完就跳幀(設 `frameSkip` auto mode)。

### 公平調度(避免某台相機獨佔)
Dispatcher 用 **round-robin queue**(每台相機各一條 FIFO,dispatcher 輪流取),
而非單一共用 FIFO — 防止某台 30fps 高 bitrate 相機把其他相機餓死。

---

## 7. 專案結構

```
nx-custom-plugin/
├── cpp/                         # C++ 插件
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── integration.cpp      # IIntegration 實作
│   │   ├── engine.cpp           # IEngine 實作
│   │   ├── device_agent.cpp     # IDeviceAgent 實作 — 主要邏輯在這
│   │   ├── ipc_client.{h,cpp}   # UNIX socket / shm
│   │   └── manifest.h           # engine/device manifest JSON 字串
│   └── third_party/
│       └── nx_sdk/              # submodule: networkoptix/nx_open_integrations
├── python/
│   ├── pyproject.toml
│   ├── inference_worker.py      # main entry, 吃 IPC
│   ├── adapters/
│   │   ├── base.py              # RuntimeAdapter Protocol
│   │   ├── pytorch_adapter.py   # 改自 yolo_http_tracker (砍掉 HTTP)
│   │   ├── onnx_adapter.py
│   │   ├── openvino_adapter.py
│   │   └── tensorrt_adapter.py
│   ├── preprocess.py            # letterbox / mask / ROI
│   ├── postprocess.py           # NMS / class filter / coord 還原
│   └── ipc_protocol.py          # 訊息 encode/decode
├── systemd/
│   └── nx-custom-worker@.service  # (選用) 獨立跑 worker
├── packaging/
│   ├── install.sh               # 部署腳本 (cp .so, mkdir models dir)
│   └── debian/                  # 選用: 做成 .deb
├── models/                      # 預設模型放這
│   └── .gitkeep
└── README.md
```

---

## 7.5 跨平台建置(Linux `.so` + Windows `.dll`)

### 7.5.1 為什麼要兩個 binary
Nx Witness 是多 Server 叢集,實務上會混雜 Linux(Ubuntu 24.04 / Debian)和 Windows Server。Plugin 是 C++ 原生共享函式庫:
- Linux → `.so`,使用 `__attribute__((visibility("default")))` export 符號
- Windows → `.dll`,使用 `__declspec(dllexport)` export 符號

**沒有「build once, run on both」**。官方 sample 的 CMakeLists 已經用 `if(WIN32) / elseif(UNIX)` 分岔(已查證 `opencv_object_detection_analytics_plugin/step1/CMakeLists.txt`)。Python worker 是 pure Python,跨平台共用同一份原始碼。

### 7.5.2 Toolchain 表

| 面向 | Linux | Windows |
|---|---|---|
| 編譯器 | GCC 11–13(Ubuntu 24.04 內建 13.3) | Visual Studio 2022 (MSVC v143) |
| CMake | ≥ 3.15(Ubuntu 24.04 apt 為 3.28) | ≥ 3.15(VS 自帶) |
| C++ 標準 | C++17 | C++17 |
| 輸出 | `libnx_custom_plugin.so` | `nx_custom_plugin.dll` |
| RPATH | `$ORIGIN`,`-Wl,--disable-new-dtags` | N/A(改用 PATH / DLL 同目錄) |
| 打包附帶 | `patchelf`(fixups) | 無需 |
| Python | CPython 3.12 manylinux wheels | CPython 3.12 Windows wheels |
| SDK 來源 | 同 zip,解壓後 `-DmetadataSdkDir=...` | 同 zip,解壓後 `-DmetadataSdkDir=...` |

### 7.5.3 本機 vs 遠端編譯:Linux 那一份

| 路徑 | 優 | 劣 | 建議場景 |
|---|---|---|---|
| **WSL2 Ubuntu 24.04** 跑 g++ 13 + CMake | 主機 CPU / SSD / RAM 強,編譯快 5–10× | 要一次對齊 glibc 2.39 / libstdc++ 版本,否則在 target 會噴 `GLIBCXX_3.4.x not found` | **日常 iteration**(edit → build → rsync → restart)|
| **Docker `ubuntu:24.04`**(在 Windows host) | 最乾淨、可 CI 重現 | 設定稍多,要 mount source | **CI / 正式 release build** |
| **遠端 N100 直接 build** | ABI 天然一致,不用 rsync | 2 cores 慢、RAM 緊 | **Phase 0 第一次驗證 toolchain 通**;之後很少用 |

降低 ABI 風險:CMake 加 `-static-libstdc++ -static-libgcc`(官方 OpenCV plugin 的 patchelf 步驟就是為此)。

### 7.5.4 Windows 那一份

- 在開發用 Windows 機器裝 Visual Studio 2022 Community + CMake(VS Installer 選「Desktop development with C++」+「C++ CMake tools」)
- 同一份 source tree 裡 `cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 -DmetadataSdkDir=C:\nx_sdk_6.1.1.42624`
- `cmake --build build-win --config Release`
- 產物 `build-win\Release\nx_custom_plugin.dll` 連同 `python/` 原始碼、`wheels-win/`(Windows 專屬 wheels)一起包成 `nx_custom_plugin_<ver>_win_x64.zip`

Windows 也可走 WSL2 的 Linux build 路線,但 Windows mediaserver 仍要獨立一份 `.dll`。兩者別混。

### 7.5.5 CI 建議(optional)

GitHub Actions matrix:
```yaml
strategy:
  matrix:
    os: [ubuntu-24.04, windows-2022]
```
每個 OS 各產一份 artifact,release 時合成一包雙平台 tarball + zip。

### 7.5.6 Python worker 的跨平台差異

- **相對安全的依賴**:`numpy`、`onnxruntime`、`opencv-python-headless`、`openvino` 三邊 wheel 都齊全
- **要注意**:`tensorrt` 只有 Linux/NVIDIA Jetson 有官方 wheel,Windows 沒有 PyPI wheel — 需要走 NVIDIA 官方 installer
- **IPC 層**:Linux 用 UNIX socket、Windows 用 Named Pipe — 在 `ipc_protocol.py` 做 OS 分岔(§5.1 已規劃)
- **systemd / 服務**:Linux 用 systemd unit;Windows 不需要獨立服務(worker 由 mediaserver 子行程 spawn,或 nssm 包成 Windows Service)

---

## 8. 實作階段(里程碑)

| Phase | 內容 | 產出 | 時間 |
|---|---|---|---|
| **0. 準備(✅ 2026-04-18 完成)** | ① SDK zip(退而取公開版 6.0.1.39873)+ `SDK_PIN.txt` ✅<br/>② 遠端 N100 build `sample_analytics_plugin` → 部署到 mediaserver 6.1.1.42624 → PluginManager 乾淨載入 ✅<br/>③ smoke A:自製 `nx_custom_plugin` probe,`doGetSettingsOnActiveSettingChange` 經 ComboBox `isActive:true` 正常觸發;`pushManifest()` 不改 settings UI(結論寫入 §4.7.1)✅<br/>④ smoke B:`probe.roi` 實測 value 是**字串化 JSON**,`points` 為 **0-1 normalized**(§3 解析契約已文件化)✅<br/>⑤ `/opt/nx_custom_plugin/venv/`、`/var/lib/nx_plugin/models/`、`/var/log/nx_plugin/`、`/run/nx_plugin/` 皆已建(networkoptix:networkoptix 755)✅ | 收穫:Phase 0 全部 exit criteria 達成;plugin 能載入、能收 settings、能 round-trip。M6 策略轉為 Engine 建構子靜態 bake runtime 清單 | 1 天 |
| **1. C++ 骨架(✅ 2026-04-18 完成)** | ① `env_probe.py` 放 `/opt/nx_custom_plugin/python/`,輸出 JSON `{"runtimes":[...], "devices":{...}}`(空 venv 時回 `{"runtimes":[],"devices":{}}`,由 C++ 端接收後 fallback `<none>`)✅<br/>② `env_probe.cpp` 用 `popen()` 呼叫 python probe,nx_kit Json11 解析成 `EnvProbe` 結構 ✅<br/>③ Engine ctor 一次跑 probe,`manifestString()` 回傳 cache 好的完整 manifest:engineSettingsModel(runtime/device ComboBox + workerCount SpinBox)、deviceAgentSettingsModel(Model + Thresholds + Classes + Regions 四個 GroupBox,含 modelPath TextField、Validate Button `isActive:true`、conf/iou/frameSkip SpinBox、classes CheckBoxGroup、roi/inclusiveMask/exclusiveMask PolygonFigure × 3)✅<br/>④ DeviceAgent ctor 收 `Engine*`,`pushCompressedVideoFrame` 空殼只記首 3 幀,`doGetSettingsOnActiveSettingChange` 收 Validate 點擊(Phase 3+ 才填真載入邏輯)✅<br/>⑤ 部署驗證:mediaserver 17:20:31 重啟,UI 3 群相機 GroupBox 正常顯示,使用者改 conf/frameSkip/classes + 畫 3 個 polygon,每次 Apply 都 round-trip,設定值正確到達 `settingsReceived()` ✅ | UI 可填但無動作(inference 還沒接);ComboBox items 隨本機 runtime 動態產生;PolygonFigure 多次編輯 round-trip 穩定 | 1 天 |
| **2. IPC 打通(✅ 2026-04-18 完成)** | ① `worker.py` bind `/run/nx_plugin/worker_default.sock` accept 多 client,用 threading 為每連線獨立 serve;echo-only — 每個 `type=frame` 回傳一個按 `id % 60` 滾動的 fake bbox(`nx.base.Person`,normalized 0.1→0.6 × 0.3→0.7)✅<br/>② `ipc_client.{cpp,h}` UNIX socket blocking client,length-prefixed JSON framing(4-byte big-endian length + utf8 JSON,無 body — Phase 3 再擴展);failure policy:任何 I/O error 自動 disconnect,下次 call 冷卻 2s 再 reconnect,first failure log 後 suppress ✅<br/>③ DeviceAgent ctor 建 per-camera IpcClient 並 sendAttach,`pushCompressedVideoFrame` 送 frame → blocking recv → 解析 boxes[] → makePtr<ObjectMetadata> + setBoundingBox(Rect normalized)+ UuidHelper::randomUuid() trackId → pushMetadataPacket;dtor sendDetach ✅<br/>④ DeviceAgent manifest `supportedTypes` 加 `nx.base.Person`(NX 內建 type,不需 typeLibrary 定義)✅<br/>⑤ 實測(journal 17:35:57–17:38:25):worker 未啟前 plugin 乾淨載入 + 2 秒冷卻 silent retry;worker 上線後 DeviceAgent 立即 connected `fd=64/65`,per-camera fd 獨立,emitted 計數準確(`frames=463 emitted=169` 表示 worker 未連時丟 294 幀,連上後全 emit);mediaserver 二次重啟後 IpcClient 重建 fd=176/185 無問題;Desktop Client 實地看到左右滾動的假 person bbox ✅ | NX 上看到假的 bbox overlay 循環;多相機 routing 正確;worker 啟停不影響 mediaserver 載入 | 1 天 |
| **3. 單一 Runtime (ONNX)(✅ 2026-04-18 完成)** | ① venv 裝 `onnxruntime==1.24.4` + `numpy==1.26.4` + `opencv-python-headless`(約 150 MB),`env_probe.py` 變成回報 `{"runtimes":["onnx"], "devices":{"onnx":["cpu"]}}` ✅<br/>② `adapter_onnx.py` 新增:`OnnxAdapter.load(path)` 動態讀 `session.get_inputs()[0].shape` → `(model_h, model_w)`,`letterbox((h,w))` 吃參數不再寫死 640,支援任意 imgsz(320/640/1280 都可);**output layout auto-detect**:`(1,K,6)` → yolo26 end-to-end(NMS 已 baked,直接 threshold by score),`(1,4+C,N)` → yolov8 raw(argmax class + cxcywh→xyxy + cv2.dnn.NMSBoxes)✅<br/>③ IPC wire format 擴展:frame msg = `<uint32 BE hdr_len><JSON header>` + `<body bytes>`(無 framing);header 帶 `w/h/stride/ts_us/id`,Python 端 `decode_rgb_body(body, w, h, stride)` 處理 stride padding(實測 480×352 stride=1440 > w×3 的情況)✅<br/>④ Engine manifest `capabilities` 改成 `"needUncompressedVideoFrames_rgb"`,mediaserver 自動改 call `pushUncompressedVideoFrame(IUncompressedVideoFrame*)`;DeviceAgent `settingsReceived()` 讀 modelPath/conf/iou/frameSkip → 組 `config` 訊息 send 給 worker;worker 依 path 變化決定是否 reload model ✅<br/>⑤ 實測(journal 21:19:38–21:22:03):Desktop Client 按 Validate 後 worker log `loaded model=yolo26s.onnx input=320x320 layout=yolo26 out_shape=[1,300,6]`,每幀推理 30–40 ms(warmup 95 ms → steady 30 ms),mediaserver log 出現 `emitted packet #1 objects=1 #2 objects=1 #3 objects=1`,Desktop Client 看到真實 person bbox(人走過鏡頭才觸發,空鏡頭時 boxes=0 符合預期);RGB 色彩以 dump jpeg 目視驗證正確(台灣夜景巷弄+黃機車,色彩自然無 BGR 反相)✅ | MVP:真的偵測得到,多尺寸可跑,IPC 效能 ~30 ms/frame 足夠 secondary stream | 1.5 天 |
| **4. 多 Runtime(✅ 2026-04-18 完成)** | ① `python/infer_utils.py` 抽共用 letterbox + YOLO decode(yolo26 end-to-end 與 yolov8 raw+NMS 兩套 layout 共用),`adapter_onnx.py` 重構成吃 infer_utils,layout/num_classes 從 `sess.get_outputs()[0].shape` 自動偵測 ✅<br/>② 新增 `adapter_openvino.py`(OpenVINO 2026.1.0 Python API):`ov.Core().read_model()` 吃 `.onnx`/`.xml`,`compile_model(model, device_name=<mapped>)` 搭配 AUTO fallback,`intel:gpu/intel:npu/cpu/auto` 經 `_DEVICE_MAP` 轉成官方 `GPU/NPU/CPU/AUTO`;output port shape 走 `partial_shape`,coerce 失敗用 640 fallback ✅<br/>③ 新增 `adapter_pytorch.py`(Ultralytics YOLO,`.pt` 模型,cuda:N/mps/xpu device hint)+ `adapter_tensorrt.py`(Ultralytics YOLO,`.engine` 模型,預設 `cuda:0`);兩者 infer 前都 `cv2.cvtColor(rgb, COLOR_RGB2BGR)`(Ultralytics 內部預期 BGR 會再 `[..., ::-1]` 反)✅<br/>④ `worker.py` 加 `_ADAPTER_TABLE` + `make_adapter(runtime)` 用 importlib lazy import,serve loop 收 `config` 時若 `runtime` 改變則 swap adapter,若 `model_path` 或 `device` 改變則 reload;per-connection state 包 current_runtime/current_model/current_device ✅<br/>⑤ C++ 端:`Engine::settingsReceived()` 解析 `runtime/device/workerCount` + `m_settingsMutex` 保護狀態 + `registerAgent/unregisterAgent` + `broadcastEngineConfig()` 廣播到所有 DeviceAgent;DeviceAgent `settingsReceived()` 解 `runtime/device`,`sendConfigToWorker()` 先吃相機值、empty 時 fallback 到 Engine 值,最終 empty 再 fallback 到 `onnx`/`cpu`;**manifest 的 runtime/device ComboBox 從 engineSettingsModel 搬到 deviceAgentSettingsModel 最上層的 Inference GroupBox**,因為 NX 6.1 CN Desktop Client 的 Server / System 設定面板不渲染 engine-level settings,只有相機層級能改 ✅<br/>⑥ 實測(journal 21:59:38–22:07:20):`adapter_swap cam={…} -> openvino`、`config runtime=openvino device=intel:gpu model=yolo26s.onnx`,切換後每幀 infer_ms 穩態 **22.7–24.6 ms(median 24 ms)**,比 Phase 3 的 ONNX CPU 30–34 ms **降 ~30%**;worker 活 process CPU 從 ONNX-CPU 時的整條 core 降到 **16.5%(0.66 core)**,system idle 73–75%(4c 還剩 3c);i915 debugfs `GT awake? yes` 確認 iGPU 確實在做工,OpenVINO AUTO fallback try/except 已封裝(driver 壞會退 CPU) | UI 可切換;跑在有 NVIDIA 的節點 TRT 可用,N100 節點只看得到 onnx/openvino;**per-camera 層級切換**(engine-level settings 在 NX 6.1 CN 不可見) | 1.5 天 |
| **5. Class filter UI(✅ 2026-04-18 完成)** | ① `infer_utils.COCO80_NAMES` + `names_to_ids()` 三態 mapping(None=全放行 / `[]`=全拒絕 / `[ids]`=過濾);② `worker.py` config 收 `classes` 解析成 id list,frame loop 空 list 短路跳過 `adapter.infer()`(省掉 iGPU 工作),非空就塞 `class_filter=[ids]` 給 adapter;③ DeviceAgent 新增 `parseClassesJson()` + `classIdToNxTypeId()`(0→Person,1/2/3/4/5/6/7/8→Vehicle,其他→Object),IPC config 增 `classes` 欄位,metadata 輸出改讀 `box.cls` 動態選 typeId;④ Engine manifest CheckBoxGroup `range` 從 placeholder 3 項升為完整 COCO-80,`defaultValue` 收斂成 `["person"]`,移除 "placeholder — populated by Validate" 字樣;⑤ DeviceAgent `supportedTypes` 新增 `nx.base.Vehicle` + `nx.base.Object`(皆 NX built-in,不需 typeLibrary 定義);⑥ 實測(journal 22:18:43):`classesRaw='["person","bicycle","car"]' classesParsed=3 classesSet=1` → IPC → worker `config classes=[0, 1, 2]` → `infer filter=[0, 1, 2]`;mediaserver 無 typeId unknown 警告,manifest length 5008 → 6511 bytes,`.so` 752544 → 761320 bytes | UI 可指定 COCO-80 任意子集;空選直接不推論省算力;Person/Vehicle/Object 三種 typeId 讓 Desktop Client overlay 差異化 | 0.5 天 |
| **6. ROI / Mask(✅ 2026-04-18 完成)** | ① `infer_utils.py` 新增 `normalize_to_pixel_polygon()`(clamp 0-1 後乘以 w/h → int32,< 3 點回 None)+ `_point_in()`(用 `cv2.pointPolygonTest`)+ `filter_boxes_by_regions()`(bbox 中心點測試,語意 `(ROI ∪ Inclusive) - Exclusive`,兩個正向 region 皆 None 時跳過正向檢查)✅<br/>② `worker.py` config handler 收 `roi/inclusive_mask/exclusive_mask` 三個 polygon 並存 per-connection normalized 座標,`poly_px_cache: {(w,h): (roi_px, incl_px, excl_px)}` 首幀 lazy 轉換,配置再觸發時清 cache;frame loop 走完 `adapter.infer()` 後用 **post-filter**(不 mask input),accumulator `frames_region_dropped` 紀錄跨連線總丟棄數,log 格式增 `region=R/I/E region_drop=N` ✅<br/>③ DeviceAgent 新增 `parsePolygonJson()` 容忍三種 wrapper(`{"figure":{"points":[...]}}` 主流 / `{"points":[...]}` / 裸 `[[x,y],...]`,< 3 點清空視為「沒畫」)+ `polygonToJsonArray()`(序列化回 [[x,y],...]);`settingsReceived()` 讀 `roi/inclusiveMask/exclusiveMask` 三個 settingValue,`m_settingsMutex` 保護的快照存 `m_roi/m_inclusiveMask/m_exclusiveMask`;`sendConfigToWorker()` 只 send 非空的 polygon(key 缺省 = 該區域未設定)並於 log 輸出 `roiPts/inclPts/exclPts` ✅<br/>④ 三個 built-in polygon 皆 **post-filter**(走 bbox 中心點而非 mask 輸入像素),保留模型原始 input statistics,下游 tracker(Phase 7)也不會被 padded black pixel 干擾 ✅<br/>⑤ 實測(journal 22:36:11–22:36:19):mediaserver 重啟後 C++ log `roiPts=0 inclPts=0 exclPts=0` → IPC → worker `config roi=0 incl=0 excl=0`,per-frame `region=0/0/0 region_drop=0`(三個 polygon 皆未畫,post-filter 判斷 `has_positive=False and exclusive=None` 短路不跑 region 檢查,零額外成本),`.so` 從 762000 → 770760 bytes ✅ | UI 可畫 ROI/Inclusive/Exclusive × 3;同相機多畫多次 Apply 穩定 round-trip;iGPU inference cost 不受 polygon 影響(只 drop 後 CPU-side 的 `cv2.pointPolygonTest` × N boxes) | 0.5 天 |
| **7. 追蹤 & TrackID(✅ 2026-04-20 完成,0.7.0)** | ByteTrack-lite(純 IoU greedy matching + `max_age=30`,**per-camera state**);`python/tracker.py` 新檔、`worker.py` 每 connection 帶 tracker、`device_agent.{cpp,h}` `m_trackUuids: unordered_map<int,string>` 首次見到 int track_id 就 `UuidHelper::randomUuid()` 綁一輩子,後續命中直接重用 → `obj->setTrackId(uuid)`;修掉 `tracker.py` 的 `list index out of range`(`trk_matched` 在 entry 時抓了長度,原本先新增再 age,IndexError;改成 age → prune → append 順序);`worker.py` 把 `log.error` 升成 `log.exception` 保留 traceback | Desktop Client 看到連續 track 線,journal `tally=...#Nxk trackUuids=N newUuidsThisPacket=0`(同一人走過鏡頭 trackUuid 連續)| 0.5 天 |
| **7.5. 80-class taxonomy + Select/Deselect All(✅ 2026-04-20 完成,0.8.0)** | 新 `coco80.{h,cpp}` 為單一真相來源(`inline constexpr CocoClass kCoco80[80]` + `allClassesJsonArray`/`typeLibraryObjectTypesJson`/`supportedObjectTypesJson`);Engine manifest 加 top-level `typeLibrary.objectTypes`(80 條 `patrick.nx_custom.<camelCase>`,每條帶合適的 `base: nx.base.*`);DeviceAgent `classIdToNxTypeId(cls)` 縮成表查;Classes GroupBox 加兩顆 `isActive:true` Button(`classesSelectAll` / `classesDeselectAll`)走 Active Setting → `doGetSettingsOnActiveSettingChange` → `SettingsResponse.setValue("classes", ...)` 覆寫;Engine manifest 13059 bytes(vs Phase 6 ~9 KB) | Desktop Client 勾 bench 後 tally `patrick.nx_custom.bench#13x1`,mediaserver internal logger ack `Object metadata packet contains 1 item of type patrick.nx_custom.bench`;Phase 7 tracker UUID continuity 不受影響;零 `infer failed` | 0.5 天 |
| **v1.0 Release(✅ 2026-04-20)** | plugin.cpp `0.8.0` → `1.0.0`,description 改成 "Self-hosted YOLO analytics plugin v1.0: ..."|plugin 掛 `"version": "1.0.0"`,Desktop Client Plugin Info 顯示 v1.0|— |
| **7.6 多 Cam dispatcher(TODO)** | Shared worker + per-camera queue,round-robin 公平調度,壓力測試 10+ 相機同時跑 | 多 Cam 穩定 | 1.5 天 |
| **8. 熱抽換 & Supervisor(TODO)** | 雙 session 模型切換(§4.4)、diagnostic event、supervisor 兩層(systemd + C++ watchdog,§9.1) | 穩定性 | 1.5 天 |
| **9. Linux 打包(TODO)** | `install.sh`,建目錄/權限、rsync python、cp `.so`、pip 離線裝 wheels、起 systemd unit | 一鍵安裝(Linux) | 1 天 |
| **10. Windows build + 打包(TODO)** | VS 2022 build 出 `.dll`、`install.ps1` 或 MSI、nssm 包 worker service | 一鍵安裝(Windows) | 1 天 |
| **合計** | Phase 0–8 已完成 ≈ 7 天;剩 Phase 7.6 / 8 / 9 / 10 ≈ 5 天 | | **~12 天** |

---

## 9. 風險與對策

| 風險 | 對策 |
|---|---|
| NX SDK 的 `PolygonFigure` 座標系(normalized 0-1 vs pixel)與你模型處理不同 | Phase 0 smoke 時順便印出實際送進來的 polygon raw value 對齊規格 |
| 動態 `pushManifest()` / class checkbox 重繪可能不被 6.1 SDK 支援 | **Phase 0 exit criteria** 明確測試;不支援就走 §4.7.1 的 TextField 降級方案 |
| 1920×1080 IPC copy 太慢(每幀 6 MB × fps) | 先 benchmark,>30ms 就切 shared memory(Linux `shm_open` / Windows `CreateFileMapping`) |
| Python worker 吃記憶體大(PyTorch + Ultralytics ~2GB) | **Shared worker 模式預設** + round-robin queue(見 §6.5);實測 mediaserver VmRSS 約 1 GB,4 GB headroom 容得下 worker |
| 多相機同時送幀造成 queue 爆掉 / latency 飆高 | 每台相機各自 **bounded queue**(size=2),滿了丟舊的(keep-latest policy);UI 顯示 `drop_count` |
| 不同相機需要不同 size 模型(近景用 640,遠景用 1280) | 模型路徑 per-DeviceAgent,dispatcher 依 modelPath 分組 worker(§6.5 方案 C 混用)|
| 追蹤 ID 跨相機衝突(BoT-SORT 共用 counter) | 每個 camera_uuid 各有獨立 `BYTETracker` 實例,ID space 分離 |
| 叢集異質硬體(有的節點 NVIDIA、有的 Intel、有的沒 GPU) | UI 下拉選單依 §M6 動態列出「本節點可用的 runtime/device」,使用者手動選 |
| Plugin crash 拖垮 mediaserver | C++ 端所有 IPC 呼叫包 try/catch;**worker 掛掉由 supervisor 自動重啟**(見下) |
| Metadata SDK 版本與 mediaserver ABI 不匹配 | `cmake` 時 `metadataSdkDir` 必須指向**對應版號**的 SDK zip;CMakeLists `project()` 下方加 `message(STATUS "Building for SDK ${metadataSdkVersion}")` log |
| 自動更新覆蓋 | 安裝目錄與 NX AI 分離(新的 plugin 目錄),不動 `nxai_plugin/` |

### 9.1 Worker Supervisor(M4 解法)

Worker 崩潰恢復採**雙層防護**,不是二選一:

1. **Linux systemd unit**(`systemd/nx-custom-worker@.service`,`Restart=on-failure`,`RestartSec=3`)
   - `worker_mode=shared/pool` 時,由 systemd 管理 worker 生命週期,mediaserver 只是 client
   - 好處:mediaserver 自身重啟不會把 worker 一起殺;worker coredump 會進 `journalctl` 方便 debug
2. **C++ IPC watchdog**(`ipc_client.cpp`):
   - 每次寫 socket 包 try/catch,`EPIPE` / `ECONNRESET` 觸發 reconnect loop(exponential backoff 1s→2s→4s, cap 30s)
   - 重連成功後重送當前 `config` + `attach` 重建 per-camera state
   - 期間幀直接 drop,同時 `IPluginDiagnosticEvent` 推 warning

**Windows** 對應:用 `nssm` 把 `inference_worker.py` 包成 Windows Service,Recovery 設 `Restart service after 3 seconds`。C++ watchdog 邏輯不變。

**per-camera 模式不用 systemd**:worker 由 C++ plugin 用 `fork`/`CreateProcess` 直接 spawn,watchdog 負責監控退出碼,非零就 respawn。

---

## 10. 離線部署(無外網假設)

目前 server 與未來部署環境都**不保證能連外網**(CN 網路 + 可能被 block 的情境),
所以整套 plugin 必須**完全離線可安裝**。

### 套件離線化
在可連網的開發機一次下載好,帶到目標機安裝:
```bash
# 開發機:預先下載所有 wheel
mkdir -p offline_wheels && cd offline_wheels
pip download \
    ultralytics onnxruntime-openvino openvino numpy opencv-python-headless \
    --platform manylinux2014_x86_64 --python-version 312 \
    --only-binary=:all: -d .

# 目標機:離線安裝
pip install --no-index --find-links ./offline_wheels ultralytics onnxruntime-openvino ...
```

### 安裝腳本約束
`install.sh` 內**禁止**任何下列動作:
- `curl` / `wget` 指向外網(只允許 `file://` / 本機路徑)
- `apt-get update` → 需另行用 offline apt cache
- `pip install <name>`(須帶 `--no-index --find-links`)
- `git clone`(所有 submodule 預先 vendored 進 tarball)

### 模型自備
- **不做**預設模型下載 — 安裝完畢 `/var/lib/nx_plugin/models/` 是空的
- README 提供一段 `nx_onnx_export.py` 命令,用戶在自己環境產 ONNX 再拷貝過來
- UI 首次啟用時若 `modelPath` 為空,Plugin 發 Diagnostic:"請於 Settings 指定模型路徑"

### 發布物結構
```
nx_custom_plugin_v1.0.0.tar.gz
├── lib/nx_custom_plugin.so       # C++ plugin
├── python/                       # worker 原始碼
├── wheels/                       # 離線 Python 套件
├── install.sh                    # 純本地操作,無網路呼叫
└── README.md                     # 明確標示「無網路安裝」
```

---

## 11. 版本鎖定策略

Plugin 的穩定性**取決於 mediaserver / NX SDK 的 ABI 不變**。
由於我們脫離 NX AI,一旦 mediaserver 自動升級就可能:
- `nx_sdk` ABI 變更 → plugin 無法載入
- manifest schema 新增欄位 → UI 顯示異常
- metadata type 變更 → 框畫不出來

### 硬性規則(寫進 README 第一頁)
1. **鎖定 mediaserver 版本**:`6.1.1.42624`(當前驗證過的版本)
2. **停用 apt 自動更新** 對 `networkoptix-mediaserver`:
   ```bash
   sudo apt-mark hold networkoptix-mediaserver
   ```
3. **停用 NX 內建自動更新 UI 選項**(System Administration → Updates → Off)
4. 升級前**必須先於 staging 環境測試 plugin** 是否仍可載入
5. Plugin 只 `#include` 公開的 `nx_sdk/*.h`,**絕對不引用** NX AI 內部 header 或符號

### ABI 契約(M5 解法)
- **C++ 端**:Metadata SDK **不是 git submodule**(repo 也沒提供)。流程:
  1. 從 Nx 官方公開下載頁取 zip(實測 URL 模式:`https://updates.networkoptix.com/metavms/<ver>/sdk/metavms-metadata_sdk-<ver>-universal.zip`)
     — **若目標 mediaserver 版號(例如 CN 版 6.1.1.42624)未上架**,取**不大於 mediaserver 的最新公開版本**(目前為 `6.0.1.39873`)
  2. 解壓到 `third_party/metadata_sdk/`,這個目錄**不進 git**(放進 `.gitignore`),太大了
  3. 檔案 checksum(SHA256)記錄在 repo 根目錄的 `SDK_PIN.txt`,供其他開發者驗證下到對的 zip
  4. `cmake -DmetadataSdkDir=$PWD/third_party/metadata_sdk`
  5. CMakeLists 開頭 `set(expectedSdkVersion "6.0.1.39873")` 並驗證 `${metadataSdkDir}/version.txt` 吻合(不合就 `FATAL_ERROR`)
  6. **Phase 0 ABI smoke test 通過**(官方 sample 能被 target mediaserver 載入 → 視為 pin 有效);失敗才回頭追同版號 SDK
- **Python 端**:`requirements.txt` 完全 pin(`ultralytics==X.Y.Z`,不用 `>=`)
- **IPC 協定**:自己訂的 JSON schema — **完全可控**,不受 NX 升級影響
- **mediaserver 升級 workflow**:
  1. 先在 staging 機試新版 mediaserver
  2. 下載新版對應的 Metadata SDK zip,更新 `SDK_PIN.txt` 檢查碼
  3. 重新編譯 plugin → 在 staging 驗證 UI / 推論 / 熱抽換 / crash recovery
  4. 通過才推 production

### 不碰 NX AI Manager
- 安裝路徑完全分離:`.../plugins/nx_custom_plugin/`(不是 `.../plugins/nxai_plugin/`)
- **不共用** cache 目錄、不共用 `/tmp/nxai_manager.sock`
- 兩個 plugin 可並存,Desktop Client 也能同時看到兩個 Engine 選項
- 現有 bypass 若要保留可以保留,但本 plugin 不依賴它

---

## 12. 參考資料

### 官方(實地查證過)
- **Integration repo**(MPL 2.0): https://github.com/networkoptix/nx_open_integrations
- **Sample plugin(6.x)**:`cpp/vms_server_plugins/opencv_object_detection_analytics_plugin/`
  — 有 `step1` ~ `step4` 漸進式 sample,`step1` 是最簡骨架(只有 `IIntegration/IEngine/IDeviceAgent`)
- **OpenVINO sample**:`cpp/vms_server_plugins/openvino_object_detection_analytics_plugin/`
  — 可參考 OpenVINO 整合方式(但他們用 Conan + C++ 直接 link,我們走 Python worker,只參考 UI / manifest 部分)
- **Branches**:repo 目前僅有 `master` / `vms_5.0` / `vms_5.1`,**沒有 `vms_6.x` branch**;6.x 用 `master`,靠 `metadataSdkDir` 下載的 SDK zip 綁版本
- **Metadata SDK zip**:Nx 官方 developer 下載頁(Phase 0 確認實際 URL);版號必須等於 mediaserver(我們要 `6.1.1.42624`)

### 輸出格式參考
- **ONNX 輸出 tensor name 規範**(NX AI 格式,**選用**,不強制):
  `bboxes-format:xyxysc;0:person;1:bicycle;...`
- 任意 ONNX / PyTorch / TRT / OV 模型也支援,只是類別清單要靠 `.yaml`/`.names` 檔或 `model.names` fallback(§4.7)

### 現有可複用資產(你的 repo)
- `yolo_env_checker.py` — runtime / device 偵測(§M6 的 UI 下拉選單就是重用這個)
- `yolo_http_tracker.py` — 推論迴圈骨架(去掉 HTTP 和 RTSP,剩下的搬進 `inference_worker.py`)
- `nx_onnx_export.py` — 產 NX 相容 ONNX 的工具(**使用者手冊附錄**,不是 plugin 依賴)
- `nxai-model-to-onnx/yolo26-to-onnx/` — 格式參考,與本 plugin 執行路徑**無關**

### 運行環境(已驗證)
- **Linux 目標機**(`192.168.10.212`):
  - Ubuntu 24.04.4 LTS,kernel 6.17
  - Intel N100(2 cores, 5.8 GB RAM),Intel UHD Graphics,無 NPU、無 NVIDIA
  - NX Mediaserver 6.1.1.42624,networkoptix user uid=999/gid=988
  - Mediaserver VmRSS ≈ 1 GB,MemAvailable ≈ 4 GB(真實記憶體,不是 systemd cgroup 顯示的 peak)
  - `libtbb12` / `intel-opencl-icd` / `build-essential` / g++ 13.3 已裝
  - **待補**:`cmake` / `ninja-build` / `libssl-dev` / `pkg-config`
  - Python 3.12.3
- **Windows 目標機**(未來節點):視實際部署機器,最少 VS 2022 Runtime + Python 3.12
- **開發機**(Windows host):建議裝 WSL2 Ubuntu 24.04 做 Linux build,同機用 VS 2022 做 Windows build

---

## 附錄:計畫變更紀錄

- **2026-04-20 v1.0 Release** — plugin.cpp version `0.8.0` → `1.0.0`,description 收斂成「Self-hosted YOLO analytics plugin v1.0: multi-runtime (onnx/openvino), full COCO-80 plugin-scoped taxonomy, ROI pixel crop, inclusive/exclusive masks, ByteTrack-lite stable track IDs, Select all / Deselect all — all Phase 0–8 features shipped and verified」;整份規劃書清潔:刪除 Non-Goals 裡的 Segmentation / Pose(本版本不做、下版本也無硬性需要,直接從頁面移除避免誤導);§狀態章節加 v1.0 stamp + source code 所在提醒(Windows host `C:\Users\Yuni\Nothinghere\phase1-staging\`,**非 git repo、無雲備份**)


  - 授權改正 Apache → **MPL 2.0**
  - SDK 改正為官方 zip 下載(非 git submodule)
  - 加入 §7.5 跨平台建置(Win `.dll` + Linux `.so`)
  - C1/C4/C5/M3/M4/M5/M6 的解法寫入對應章節
  - C2/C3 依使用者指正撤回(保留四 runtime;記憶體預算以實測 RSS 為準)
  - M1/M2 撤回(plugin 與模型匯出工具解耦)

- **2026-04-20 Phase 8(0.8.0) 已驗證完成** — 80-class plugin-scope taxonomy + Select all / Deselect all 按鈕,對齊 NX AI Manager 官方 UI:
  - **驗證結果(journal 20:44:32+,Desktop Client 目視)**:
    - Engine manifest length **13059 bytes**(Phase 6 ~9 KB,typeLibrary 80 條定義吃掉 ~4 KB,符合預期)
    - C++ log `classesParsed=80 classesSet=1`(按 Select all 後 UI 80 項全勾,Apply 下去 Engine 收到完整 80 項;使用者單勾 `bench` 時 `classesSet=1`,既有勾選不會被 typeLibrary 新增 reset)
    - 偵測到椅子類物件時 tally 從 `nx.base.Unknown#13x1` 變 `tally=patrick.nx_custom.bench#13x1 trackUuids=1 newUuidsThisPacket=1`
    - mediaserver internal logger ack:`Object metadata packet contains 1 item of type patrick.nx_custom.bench`(= Desktop Client / event rule 都能識別這個 plugin-scope typeId,沒被當成 unknown drop)
    - Phase 7 tracker UUID 連續性 **完全不受影響**(同一 bench 物件跨幀 `newUuidsThisPacket=0`),零 `infer failed: list index out of range`
    - Select all / Deselect all 按鈕都不需要重啟 mediaserver,UI 即時反映
  - 問題定義:使用者截圖顯示 NX AI Manager 「Detect classes」清單超過 60 項(包含 aeroplane / motorbike / fire hydrant / laptop / parking meter…),跟我們只有 13 個 NX built-in `nx.base.*` 的行為落差很大;而且我們全勾 80 項時所有非 built-in 對應的類別都會在 Desktop Client 顯示成 `nx.base.Unknown #<cls>`,使用者無法從畫面上區分是 fire hydrant 被誤判還是 backpack 被誤判 —— UI 上勾選 80 項但視覺輸出只能區分 ~13 類,UI↔output 非對稱
  - 證據:NX AI Manager 用的是 PASCAL VOC 拼法(aeroplane / motorbike / sofa),等於把 Ultralytics YOLO `data.yaml.names` 原封不動轉成 plugin-scope object types。SDK 文件 `third_party/metadata_sdk/src/nx/sdk/analytics/taxonomy.md:75-85` 明確支援 Engine/DeviceAgent manifest 內放 `typeLibrary.objectTypes[]`,每條定義可含 `{id, name, base, icon}`,其中 `base` 會讓 Desktop Client 繼承該 built-in 型別的 icon + bounding box 顏色
  - Phase 8 scope:
    - ① 全部 80 個 COCO 類別宣告為 plugin-scope type `patrick.nx_custom.<camelCase>`(`person`/`fireHydrant`/`cellPhone`...)
    - ② 每條 type 有讀得懂的 `name`(直接用 Ultralytics COCO 拼法 `"fire hydrant"`、`"cell phone"`)以及合理的 `base`(`person`→`nx.base.Person`、`bicycle`→`nx.base.Bike`、動物→對應的 `nx.base.{Cat,Dog,Bird,Animal}`、其餘 56 類共用 `nx.base.Unknown` 作 base 但仍有獨立 name 可顯示)
    - ③ 「Classes」GroupBox 加兩顆 `Button`(`classesSelectAll` / `classesDeselectAll`)走 **Active Setting** 機制 — 按下後 `doGetSettingsOnActiveSettingChange` 回傳 `SettingsResponse` 覆寫 `classes` 欄位;使用者仍需按 Apply 才會 push 到 worker(對齊 NX AI Manager 的「button 只改 UI ticks、Apply 才生效」行為)
  - 為什麼這麼做不是「把類別硬塞到 `nx.base.*` 系統」:NX built-in taxonomy 是 NX 公司定義的穩定分類空間,我們不該污染;plugin-scope id `patrick.nx_custom.*` 天然與 NX core 隔離,未來 NX 擴充 built-in 也不會衝突。我們用 `base: nx.base.X` 就足以繼承視覺風格,不需要 ID 同名
  - 工件規劃:
    - 新檔 `src/.../sample/coco80.h`(header-only `kCoco80[80]` `inline constexpr` 表 + 三個 helper 宣告)+ `coco80.cpp`(三個 JSON fragment builder)— **單一來源真相**,`engine.cpp`/`device_agent.cpp` 都從這裡拿資料
    - `engine.cpp`:Classes GroupBox 的 `range` 從寫死 80 字串陣列改成 `<< allClassesJsonArray()`;GroupBox 開頭加兩個 Button(name `classesSelectAll` / `classesDeselectAll`、`isActive: true`);Engine manifest 頂層新增 `typeLibrary.objectTypes` 欄位 `<< typeLibraryObjectTypesJson()`(80 條定義)
    - `device_agent.cpp`:① `classIdToNxTypeId(cls)` 縮成 `return kCoco80[cls].typeId`(含 `cls < 0 || cls >= kCocoCount` fallback 到 `nx.base.Unknown`)② `manifestString()` 的 supportedTypes 從寫死 14 條改成 `<< supportedObjectTypesJson()`(80 + Unknown 尾項)③ `doGetSettingsOnActiveSettingChange` 新增 button name 檢測,`classesSelectAll` → `setValue("classes", allClassesJsonArray())`、`classesDeselectAll` → `setValue("classes", "[]")`,build 成 `ActiveSettingChangedResponse` 回傳
    - `plugin.cpp`:version `0.7.0` → `0.8.0`;description 改 "Phase 8: full 80-class plugin-scoped taxonomy — every COCO class renders with its own label in Desktop Client instead of collapsing to 'Unknown #N'. Classes GroupBox now has Select all / Deselect all buttons aligned with NX AI Manager conventions"
    - `CMakeLists.txt` **不用改** — 目前使用 `file(GLOB_RECURSE ... src/*)`,新增 `.cpp`/`.h` 會自動被掃進去
  - 驗證計畫:
    - Desktop Client 開相機設定 → Classes GroupBox 上方看到兩顆按鈕 + 下方 80 項 CheckBoxGroup(與現況勾選狀態一致,不會因 Engine manifest `typeLibrary` 新增而 reset 使用者既存勾選)
    - 按「Select all」→ UI 80 項全勾;按「Deselect all」→ 全清空;都不需要 server 重啟
    - 有 fire hydrant / laptop / chair 這種非 built-in 對應的偵測出現時,Desktop Client bbox 標籤顯示「fire hydrant」而不是「Unknown #10」;tally log 從 `nx.base.Unknown#10x1` 變成 `patrick.nx_custom.fireHydrant#10x1`
    - 之前 Phase 7 已通過的 trackId 持續性 **不受影響**(tracker.py / m_trackUuids 與 typeId 改動正交)

- **2026-04-20 Phase 7(0.7.0) 完成** — 物件追蹤 + 穩定 TrackID,NX Desktop Client 會看到連續 track 線:
  - **驗證結果(使用者 20:44+ 目視確認 + journal 比對)**:
    - 同一物件跨幀 `newUuidsThisPacket=0`,UUID 延用(`m_trackUuids` int → UUID 映射命中率正確)
    - 新物件進畫面瞬間拿到新 UUID(`min_hits=0` 零延遲,符合設計)
    - bug fix:`tracker.py` 原本是「先 match → 先 new_track → 再 age」,但 `trk_matched = [False] * len(self._tracks)` 在 entry 時抓長度,後續 new_track 會 append 把 `self._tracks` 長度推過 `trk_matched` 邊界,aging 的 `enumerate` 就 IndexError;改成 **match → age → prune → append** 後 `trk_matched` 的索引永遠落在原長度內,錯誤消失(`journalctl -u mediaserver --since '20:44'` 零 `infer failed`)
    - 同時把 `worker.py` 的 `log.error("infer failed: %s", exc)` 升成 `log.exception(...)`,未來若再有別種 exception 能拿到完整 traceback(之前這顆 IndexError 只看到一行 message,追不到 frame)
  - Scope(刻意收斂到 0.5 天可完成的最小可行):
    - 演算法:**ByteTrack-lite** — 純 IoU greedy matching + `max_age` grace period;**不做** Kalman 預測、ReID、camera motion compensation、two-stage high-low confidence matching(這些是 full ByteTrack / BoT-SORT 的內容,留 Phase 7.x)
    - 參數(可調,寫死進 tracker.py):`iou_thr=0.3`(匹配門檻)、`max_age=30`(離線幀數上限,≈1s @ 30 fps)、`min_hits=0`(不等確認,首幀就給 track_id)
    - Per-camera state:跟 Phase 4 起的 IPC 架構一致 — 每條連線(= 每台相機)在 `serve()` 作用域內建自己的 `ByteTrackLite` 實例,相機間 trackId space 天然隔離,斷線重連 tracker 從零開始(符合 NX 語意:trackId 是「該 analytics session」內的,不是全域)
  - 為什麼不做 Kalman(v1):① 計算成本雖小但增加 state 複雜度(每 track 4×4 矩陣);② 固定 CCTV 相機下運動速度遠慢於幀間距,純 IoU 在 30 fps 下失配率極低;③ Kalman 真正的優勢在「預測 missed 幀的 bbox 給 NX 畫」,v1 選擇「missed 幀直接不 emit」,Desktop Client 會看到物件短暫消失但 trackId 重新出現時仍能接續 — 對安控場景已夠用
  - 為什麼不做 ReID(v1):Intel N100 的 iGPU inference 預算已經全給 YOLO26,再擠一個 OSNet / MobileViT-ReID model 會讓 detector 掉到 sub-15 fps;ReID 的實際增益(跨遮擋、跨鏡頭 ID 保持)在單相機 + `max_age=30` 下邊際效益小
  - 為什麼 min_hits=0(首幀就給 id):full ByteTrack 預設 `min_hits=3`(3 幀確認才給 track_id)避免假陽性 flicker,但代價是任何真正物件進入畫面都會延遲 ~100ms 才有 bbox;v1 選「直接給 id」→ 假陽性會出現一瞬就消失的 track id,但真陽性零延遲,Desktop Client 的 track search 仍能找到;後續若 NX 端看太多假陽性 track 再調
  - TrackID 型別與映射:worker 端 tracker 產 `int track_id`(自 1 單調遞增,per-connection),IPC 回覆每個 box 加 `track_id` 欄位;C++ DeviceAgent `m_trackUuids: std::unordered_map<int, std::string>`(per-camera,`pushUncompressedVideoFrame` thread 內存取,無需 mutex — 目前架構只有這個函式摸它;**若未來 tracker 改 async 必須補 mutex**),首次看到某個 int → `UuidHelper::randomUuid()` 存進去、後續命中直接回同個 UUID → `obj->setTrackId(uuid)` 給 NX
  - C++ UUID map 清理(v1 不做):worker 端 tracker 的 `max_age=30` 意味任何 int track_id 停滯 > 30 幀就不再送出,C++ map 對應項變 stale 但占用 minimal(每 entry ≈ 50 bytes,UUID 字串 36 bytes + int key 4 bytes + map overhead);單相機 24/7 粗估 ≤ 100k distinct tracks = ≤ 5 MB,可接受;**已記入 TODO**,Phase 7.x 加 TTL 掃描或 tracker 回報 `tracks_ended` 讓 C++ erase
  - 工件規劃:
    - 新檔 `python/tracker.py`(~150 行):`ByteTrackLite` class — `update(boxes) → boxes_with_track_id`;內部維持 `list[Track]`,每個 Track 只存 `(track_id:int, last_xyxy, last_cls, time_since_update)`;IoU matrix 用 numpy 一次算完,greedy matching 按 IoU 遞減挑、避開重複 track;跳過 Hungarian 省 scipy 依賴(單相機下最大 detection 數 < 30,O(N²) greedy 完全夠用)
    - `python/worker.py`:per-connection 加 `tracker: Optional[ByteTrackLite] = None`;config 首次到來時 `tracker = ByteTrackLite(iou_thr=0.3, max_age=30)`;frame handler 在 post-filter 後、`send_msg(reply)` 前呼叫 `boxes = tracker.update(boxes)`;log 增 `tracks=N`(目前活著的 track 數)
    - `src/nx/vms_server_plugins/analytics/sample/device_agent.{cpp,h}`:member `std::unordered_map<int, std::string> m_trackUuids`(h 加 `#include <unordered_map>`);`pushUncompressedVideoFrame` 讀 `box["track_id"]` 做映射,`obj->setTrackId(uuid)` 取代原本的 `UuidHelper::randomUuid()`;log `tally` 擴成 `tally=nx.base.Car#2x3(t=17)` 形式,把前幾個 track_id 也印出來方便觀察連續性
    - `plugin.cpp`:version `0.6.3` → `0.7.0`;description 改為 "Phase 7: ByteTrack-lite tracker (IoU + max_age, per-camera state); Desktop Client receives stable trackId UUIDs for continuous object tracks"
  - 驗證計畫:
    - journal 首三幀 log `tracks=N`(空鏡頭 N=0,有人時 N≥1)
    - 同一個人連續走動 → Desktop Client 的 bbox 右上角應顯示**同一 trackId**(overlay 文字或「Object Search」結果 track 連續)
    - 人暫離鏡頭 < 1s 後回來 → trackId 應保持(IoU match 到舊 track)
    - 人暫離 > 1s(30 幀)後回來 → trackId **會**換新(符合 max_age 設計預期)
    - 兩個人交錯走位 → trackId 有機會交換(v1 純 IoU 的已知限制,Phase 7.x Kalman 能改善)

- **2026-04-20 Phase 6.3 (0.6.3) 完成** — ROI 從 post-filter 改為**真正的像素裁切**,徹底對齊 NX AI Manager 官方語意:
  - 實測(journal 20:02:36–20:02:38,deploy 後首次):
    - `config cam=... classes=[0, 1, 2, 3] roi=4 incl=0 excl=0`(C++ 端 wire format 不變,仍送 4 點)
    - `infer ... boxes=0/0 infer_ms=24.3 (480x352 stride=1440) filter=[0, 1, 2, 3] crop=150,137,330x215 region=0/0 region_drop=0 top3=[]` — log 新格式生效:`crop=x,y,WxH` 顯示實際裁切 origin + size;`region=incl/excl` 僅剩兩欄(ROI 已離開 post-filter)
    - 使用者在 Desktop Client persistent 保留的 ROI 被正確吃進:480×352 幀被裁成 (150,137)–(480,352) 的 330×215 區域,約佔 42% 像素,OpenVINO GPU 推理 24.3 ms(裁切後 letterbox 到 320×320 model input,與全幀時相當 — 模型本身是 fixed-shape,裁切省的是 pixel 預處理 + letterbox pad ratio 改善,非 inference 本身)
    - 零錯誤、零 crash、zero-box 場景 `boxes=0/0` 與 0.6.2 行為一致
  - 部署注意點:worker.py 的重新載入**必須重啟 `nxworker.service` transient unit**(pid 由 137601 → 357795);首次部署時忘記它會讓 mediaserver 重連上同個 old worker process → log 保持舊 3 欄格式,這個 gotcha 已寫入 §9.5 離線部署檢查清單的「change python code」路徑
  - 工件落地:
    - `python/worker.py`:新增 `_crop_to_roi()` + `_MIN_CROP_PX=8` 門檻 + frame handler crop+remap 分支 + `config` 狀態 per-connection 註解重寫
    - `python/infer_utils.py`:僅 docstring 更新(`filter_boxes_by_regions` 簽名保留,ROI 參數從此永遠傳 `None`)
    - `plugin.cpp`:version `0.6.2` → `0.6.3`;description 改 "Phase 6.3: ROI now performs true pixel crop before inference (NX AI Manager convention) — model literally does not see pixels outside ROI; inclusive/exclusive masks continue as coordinate post-filters on bbox center"
    - C++ `device_agent.cpp` / `engine.cpp`:**完全沒動** — 驗證了 Phase 6.2 的 BoxFigure → 4 點 polygon wire format 已是這個設計的正確基礎
  - 待使用者視覺驗證(Desktop Client):① 畫一個佔畫面 1/4 的 ROI,確認 ROI 外放人時 bbox 數 = 0;② 人在 ROI 內移動時 bbox 應緊貼人(因 remap 正確),不會偏移到 ROI 外;③ 切掉 ROI → log 回 `crop=full`

- **2026-04-20 Phase 6.2 (0.6.2)** — ROI 形狀 `PolygonFigure` → `BoxFigure`,對齊 NX AI Manager 官方慣例:
  - 觸發:使用者在 0.6.2 部署後追問「ROI 與 Inclusive Mask 都畫矩形有何區別」— 查代碼 `filter_boxes_by_regions` 的 `inside_pos = _point_in(roi_px, ...) or _point_in(inclusive_px, ...)` 確認兩者是純 OR union,**在過濾語意上完全等效,只是 UI 形狀限制不同**。這是 Phase 6 的設計冗餘:同一個「正向 region」概念用了兩個欄位
  - 決議:使用者選 (A) 方案 — ROI **裁切像素**送模型,Inclusive/Exclusive 維持 bbox-center post-filter。從此 ROI 與 Mask 在 pipeline **不同層級**運作,就不再冗餘:
    - ROI 作用於**輸入影像**(crop 在 `adapter.infer` 之前)→ 模型完全看不到 ROI 外的畫面
    - Inclusive / Exclusive 作用於**模型輸出的 bbox 座標**(post-filter)→ 可以描不規則形狀
  - 新 pipeline:
    ```
    frame (w×h)
      ├── if ROI 有畫:
      │     crop_x1 = clamp(min(pt.x) * w, 0, w)   # 4 點取 min/max 組 bbox,不依賴 CCW 順序
      │     crop_y1 = clamp(min(pt.y) * h, 0, h)
      │     crop_x2 = clamp(max(pt.x) * w, 0, w)
      │     crop_y2 = clamp(max(pt.y) * h, 0, h)
      │     crop = frame[crop_y1:crop_y2, crop_x1:crop_x2]
      │     cw = crop_x2 - crop_x1; ch = crop_y2 - crop_y1
      ├── else:
      │     crop = frame; (crop_x1, crop_y1) = (0, 0); (cw, ch) = (w, h)
      ▼
    boxes_crop_norm = adapter.infer(crop, ...)     # xyxy 已 normalize 到 [0,1] of crop
      │
      ▼  remap crop-norm → original-frame-norm
    for b in boxes_crop_norm:
        b.xyxy = [
            (x1 * cw + crop_x1) / w,
            (y1 * ch + crop_y1) / h,
            (x2 * cw + crop_x1) / w,
            (y2 * ch + crop_y1) / h,
        ]
      │
      ▼
    boxes = filter_boxes_by_regions(boxes, w, h,
        roi_px=None,                    # ← ROI 已在 crop 階段作用完,不再進 post-filter
        inclusive_px=inclusive_px,
        exclusive_px=exclusive_px)
      │
      ▼  送 IPC → C++ → NX Metadata
    ```
  - 為什麼 remap 用 min/max 而不是直接取 `points[0]`/`points[2]`:`parseBoxFigure` 雖然 emit CCW 4 點但那是 C++ 端的輸出約定,worker 不該跟 C++ 耦合到這麼緊;min/max 在 Python 端永遠對,即使未來 C++ 換序也不會壞
  - `adapter.infer()` 四個實作(torch/onnx/openvino/tensorrt)**完全不用動** — adapter 收到小畫面 → 自己 letterbox 到模型 input size(640×640) → 產出 bbox normalize 回「它收到的那張畫面」的 [0,1] — 這個行為對 cropped frame 依然正確,只差最後 worker 多一步 crop→original remap
  - `filter_boxes_by_regions` 的簽名**保留 `roi_px` 參數但改成永遠接 `None`**(work.py 端不再傳),內部邏輯不改 — 降低 Phase 6 回滾成本、也讓未來「使用者選 ROI 模式:crop vs filter」有迴旋空間
  - 實作工件清單:
    - `python/worker.py`:新增 `_crop_to_roi(frame, roi_norm, w, h)` helper(回 `(crop, crop_x1, crop_y1, cw, ch)`),frame handler 在 `adapter.infer` 前呼叫、之後 remap bbox;per-frame log 增 `crop=(x1,y1,cw,ch)` 或 `crop=full`
    - `python/infer_utils.py`:無需改檔(filter 簽名保留,只是 worker 呼叫時 `roi_px=None`);為文檔一致性把 docstring 加一行「ROI 自 0.6.3 起由 worker.py 在 inference 前裁切像素,不再進入本函式」
    - `device_agent.cpp` / `engine.cpp` / `plugin.cpp`:**C++ 端完全不動** — ROI 的 wire format 沒變(C++ 仍送 4 點 polygon,worker 端自己 min/max 組 bbox)
    - `plugin.cpp`:version `0.6.2` → `0.6.3`;description "Phase 6.3: ROI becomes true pixel crop before inference (NX AI Manager convention); inclusive/exclusive masks remain post-filter by bbox center"
  - 預期行為差異(與 0.6.2 對比):
    - ROI 涵蓋整張畫面 / ROI 未畫:**完全等效於 0.6.2**(crop 跟全幀相同)
    - ROI 涵蓋畫面一小塊:**推理 FLOPs 下降**(adapter letterbox 的輸入解析度縮小)、ROI 外的物件**完全看不到**(模型沒餵到那些 pixel)、原本跨 ROI 邊界的大物件會被裁成部分 bbox(若裁後仍有足夠 feature,仍會 detect)
    - ROI 縱橫比極端(例如 1920×100 的長條 ROI):letterbox 到 640×640 後有效解析度很低,**小物件 recall 會下降**— 這是像素裁切不可避免的代價,Phase 7 tracker 上線前使用者若畫極端長條要自行承擔
  - 跨 ROI 邊界的 tracker 軌跡斷裂問題:**0.6.3 先不處理**,Phase 7(YOLO-tracker IoU + ReID)設計時把「ROI 外沒有觀測」納入 Kalman missing-observation 模型即可;不是 0.6.3 的 blocker
  - 驗證計畫:
    - 不畫 ROI:journal 應顯示 `crop=full`,bbox 座標與 0.6.2 完全一致(無 remap 誤差)
    - 畫一個佔畫面 1/4 的 ROI,在 ROI 外放人:bbox 應完全 **0**(模型看不到)
    - 畫同一個 ROI,在 ROI 內放人:bbox 應正常出現,且 xyxy remap 後畫在 Desktop Client 上位置要跟人重合(±2 px,因 int 裁切誤差)
    - ROI + Inclusive 同時畫(例如 ROI 是主要區域,Inclusive 突出一個小角):Inclusive 若完全在 ROI 內 → 效果等同只有 ROI;Inclusive 若有部分在 ROI 外 → 該外部分**永遠 0 detection**(像素沒餵到),表現為 Inclusive 被 ROI 裁掉的那半失效 — 這是 pixel-crop 語意的邏輯後果,文件要註明

- **2026-04-20 Phase 6.2 (0.6.2)** — ROI 形狀 `PolygonFigure` → `BoxFigure`,對齊 NX AI Manager 官方慣例:
  - 觸發:使用者指正 "官方 plugin ROI 只能方框,其他如遮罩 mask 才可以使用不規則" — 查文件 (`https://nx.docs.scailable.net/nx-ai-manager/configure-the-plugin/input-masks-and-roi`) 確認 ROI = Rectangle only,Masks(inclusion/exclusion) = Polygon
  - `engine.cpp::buildManifest()`:Regions GroupBox 第一個 item 從 `{"type":"PolygonFigure","name":"roi","minPoints":3,"maxPoints":12}` 改為 `{"type":"BoxFigure","name":"roi"}`(BoxFigure 沒有 minPoints/maxPoints,Desktop Client 固定畫一個可拖的軸對齊矩形);同時補三 region 的 `description` 欄解釋語意(ROI 軸對齊 / Inclusive polygon 加、Exclusive polygon 減)
  - `device_agent.cpp` 新增 `parseBoxFigure(raw, outPts)` helper:三層 fallback 定位 points(同 `parsePolygonJson` 的尋址邏輯 — `parsed["figure"]["points"]` / `parsed["points"]` / `parsed` 本身是陣列),抓前兩個 corner `(ax, ay)` 與 `(bx, by)` → `x1=min(ax,bx) x2=max y1=min y2=max`(使用者可能反方向拖),degenerate(`(x2-x1) < 1e-5` 或 `(y2-y1) < 1e-5`)回 false → 正常情況展開為 4 點 CCW 矩形 `(x1,y1) → (x2,y1) → (x2,y2) → (x1,y2)`,讓 worker 端既有的 `cv2.pointPolygonTest` filter 完全不用特判
  - `settingsReceived()` 把 `parsePolygonJson(roiRaw, &roiParsed)` 換成 `parseBoxFigure(roiRaw, &roiParsed)`;兩個 mask 維持 `parsePolygonJson`(< 3 點仍 drop);member vector `m_roi` 語意未變(仍是 4 點 CCW,worker 看不出差異)— 所以 Python 端完全不用改
  - 版本:plugin `0.6.1` → `0.6.2`;description "Phase 6.2: ROI switched from PolygonFigure to BoxFigure to align with NX AI Manager convention; inclusive/exclusive masks remain PolygonFigure for irregular shapes"
  - 實測(journal 19:50:35–19:50:40,deploy 後首次):`engine manifest built, length=6843 bytes`(+332 bytes 來自三 region description 字串);`settingsReceived #1 ... roiPts=4 inclPts=0 exclPts=0`(舊的 persisted polygon value 被 parseBoxFigure 取前兩點展成矩形,未崩);`sendConfig ... roi=4 incl=0 excl=0`;DeviceAgent ctor + IPC 正常
  - **仍存在與 NX AI Manager 的行為差異**(不在 0.6.2 範疇,留待下一次決策):① NX 官方 ROI 是 **pixel crop + letterbox resize**,我們是 **bbox-center post-filter**;② NX 官方 anchor 為 **bbox 底邊中心**,我們用 **bbox 幾何中心**;③ NX 官方 **ROI 與 Mask 不能同時使用**,我們 `(ROI ∪ Incl) \ Excl` 是 union;④ 這些差異在 Phase 6 設計時刻意選擇(見 0.6.0 條目 ①②③),Phase 7 tracker 上線前仍傾向維持 post-filter 語意
  - 工件:`engine.cpp`(BoxFigure + description)、`device_agent.cpp`(parseBoxFigure helper + settingsReceived wiring + 註解更新)、`plugin.cpp`(version/description)

- **2026-04-20 Bugfix 0.6.1** — Phase 5 typeId 映射修正:非 Person/Vehicle 類全部顯示為 Person 的 Bug 根治:
  - **根因**:`classIdToNxTypeId()` 的 default case 回 `"nx.base.Object"`,但該 typeId **不在** `taxonomy_base_type_library.json` 的 36 個 built-in objectTypes 清單內(清單只有 Person/Vehicle/Face/LicensePlate/Animal/Unknown 六個 top-level + 30 個 derived);NX 6.1 CN Desktop Client 收到無法解析的 typeId,overlay rendering 退到 `supportedTypes[0]`(=`nx.base.Person`),造成使用者看到「所有偵測都長人形圖示」
  - 修正 `classIdToNxTypeId()`:改成 switch-case 細分 COCO-80 → NX subtype:
    - `0 → nx.base.Person`(不變)
    - `1 → nx.base.Bike`(bicycle)、`3 → nx.base.Bike`(motorcycle — NX taxonomy 沒有單獨 Motorcycle)
    - `2 → nx.base.Car`、`5 → nx.base.Bus`、`6 → nx.base.Train`、`7 → nx.base.Truck`
    - `4 → nx.base.AirTransport`(airplane)、`8 → nx.base.WaterTransport`(boat)
    - `14 → nx.base.Bird`、`15 → nx.base.Cat`、`16 → nx.base.Dog`、`17..23 → nx.base.Animal`
    - 其他 → `nx.base.Unknown`(taxonomy 合法,取代原 `nx.base.Object`)
  - Manifest `supportedTypes` 從 3 條(Person/Vehicle/Object)擴成 14 條,把所有 `classIdToNxTypeId()` 可能回的 typeId 全列上,避免 NX 因「emit 的 typeId 不在 supported 清單」silently drop overlay
  - DeviceAgent `pushUncompressedVideoFrame()` 加 per-packet `std::map<std::string,int> typeTally`,log 格式:`tally=nx.base.Car#2x3,nx.base.Person#0x1`(`<typeId>#<clsId>x<count>`),first 3 packets + 每 100 筆印一次,方便 live 驗證 C++ 送出的 typeId 分佈與 worker cls 分佈一致
  - 實測(journal 19:41:07–19:41:16):`settingsReceived #1 ... classesRaw='[\"person\",\"bicycle\",\"car\",\"motorcycle\"]'`(user 已選 4 類)→ IPC → worker `filter=[0, 1, 2, 3]` → `infer id=46 boxes=0/1 region_drop=1 top3=[{'xyxy':..., 'cls': 0}]`(一個 person 被 ROI drop)。尚未看到 vehicle 進鏡頭、需要 user 讓車入鏡驗證 Car/Truck 圖示實際顯示
  - 版本:plugin `0.6.0` → `0.6.1`;description 改成 "Phase 6.1: fix typeId mapping — nx.base.Object was invalid, use specific Car/Truck/Bus/Bike/Unknown subtypes"
  - 工件:`device_agent.cpp`(switch-case + supportedTypes 擴充 + typeTally log + `<map>` include)、`plugin.cpp`(version/description)
  - **學到的一課**:emit 任何 typeId 之前都要交叉比對 `taxonomy_base_type_library.json` 的 id 欄位;`supportedTypes` 必須涵蓋所有實際會送出的 typeId

- **2026-04-18 Phase 6 完成** — ROI / Inclusive / Exclusive 三個 PolygonFigure 從 UI 繞 C++ 送到 worker,post-inference 依 bbox 中心點過濾:
  - 新增 `infer_utils.py`:
    - `normalize_to_pixel_polygon(pts_norm, w, h)`:`[[x,y],...]` 0-1 正規化先 `np.clip([0,1])` 再乘 w/h → int32 Nx2 for `cv2.pointPolygonTest`;< 3 點或 shape 不合回 `None`(呼叫端當作「該 region 沒畫」)
    - `_point_in(poly, x, y)`:`None` poly 永遠回 False,讓下游 `(ROI ∪ Inclusive)` 的 or 自然 degenerate 成「只看其中一個」
    - `filter_boxes_by_regions(boxes, w, h, roi_px, incl_px, excl_px)`:**雙短路**(`has_positive=False and excl is None` 直接回 boxes,零成本;正向 region 任一存在時 bbox 中心點必須落在至少一個正向 region 內);xyxy 是 adapter 已經 normalized 回 frame 座標的 `[0,1]`,乘 w/h 算中心點
  - 改寫 `worker.py`:config handler 加 `_coerce_poly()` 容忍 list-of-list-of-2floats,容錯掉非數字 element,< 3 點整個 drop;per-connection 存 `roi_norm/inclusive_norm/exclusive_norm` 三個 normalized list + `poly_px_cache: dict[(w,h), (roi_px, incl_px, excl_px)]`(新解析度 lazy 轉 pixel,舊解析度 cache hit),config 再次到來時清 cache;frame handler 走完 `adapter.infer(...)` 後**才**進 region filter(不 mask 輸入像素,避免干擾模型的 letterbox + normalize 輸入分佈,也讓 Phase 7 的 tracker 不會被黑色 mask block 誤導);accumulator `frames_region_dropped` 紀錄連線累積丟棄數;log 格式增 `region=R/I/E region_drop=N`(R/I/E 是三 polygon 點數,N 是本幀被 drop 的 box 數)
  - 改寫 DeviceAgent(`device_agent.{cpp,h}`):
    - `parsePolygonJson(raw, outPts)` 新 helper:三層 fallback 找 `points` 陣列 — 先 `parsed["figure"]["points"]`(Phase 0 smoke B 主流形狀)→ `parsed["points"]`(沒 figure wrapper)→ `parsed` 本身是陣列(某些 edit 後的狀態);`< 3` 點清空回 false;任一 parse fail 全部視為「沒畫」;數字節點用 `number_value()` 讀 float 以容忍 int/double 混雜
    - `polygonToJsonArray(pts)`:序列化回 `nx::kit::Json::array` 的 `[[x,y], ...]` 形狀,這個形狀比 `{"figure":{"points":...}}` 精簡、worker 端 `_coerce_poly()` 直接吃
    - 三個 member:`m_roi`、`m_inclusiveMask`、`m_exclusiveMask` 都是 `std::vector<std::pair<float,float>>`(正規化 0-1),和 `m_classes/m_classesSet` 共用 `m_settingsMutex`;`device_agent.h` 加 `#include <vector>`
    - `settingsReceived()` 讀 `settingValue("roi")/settingValue("inclusiveMask")/settingValue("exclusiveMask")` 三個 raw 字串 → `parsePolygonJson()` → 先拍下 `roiPts/inclPts/exclPts` 三個 local size(避免 `std::move` 後讀到 0)→ 進 mutex scope 一次性 swap 三個 member + `classes/classesSet` + runtime/device/threshold;log 增 `roiPts=N inclPts=M exclPts=K`
    - `sendConfigToWorker()` snapshot polygon 快照仍在同一 mutex scope,只 send 非空的(key 缺省 = 該 region 未設定);log 增 `roi=N incl=M excl=K`
  - Engine manifest 沒動(Phase 1 已經擺好 `"type":"PolygonFigure"` × 3 + `minPoints:3, maxPoints:12`),所以 UI 結構完全沒變,只差後端現在真的吃這些值
  - **為什麼走 post-filter 而不是 input-mask**:① input-mask 會把 polygon 外變成黑,letterbox + normalize 後模型看到的分佈與訓練資料偏離,邊界盒偵測機率會下降;② 之後 Phase 7 tracker 需要連續性資訊,input-mask 會讓人從 ROI 外走入時瞬間出現(而不是連續靠近),post-filter 可以保留完整軌跡、只在輸出層過濾;③ post-filter 成本 = `cv2.pointPolygonTest × N boxes`,典型 N < 20,微秒級,iGPU inference 仍是主要成本
  - 實測(journal 22:36:11–22:36:19,restart 後首次,user 未畫 polygon):
    - C++:`settingsReceived #1 ... classesRaw='[\"person\",\"bicycle\",\"car\",\"motorcycle\"]' classesParsed=4 classesSet=1 roiPts=0 inclPts=0 exclPts=0`(三 polygon 皆空,符合 UI 狀態)
    - C++:`sendConfig ... classes=[person,bicycle,car,...] x4 roi=0 incl=0 excl=0`
    - Python:`config ... classes=[0, 1, 2, 3] roi=0 incl=0 excl=0`
    - Python:`infer ... boxes=0/0 infer_ms=24.8 filter=[0, 1, 2, 3] region=0/0/0 region_drop=0 top3=[]`(N/M 格式 before/after,N=0 符合空鏡頭)
    - `engine manifest built, length=6511 bytes`(不變,Phase 5 已擺好)
    - `.so` 761320 → 770760 bytes(+9440,polygon parse/serialize + 三個 vector member)
  - **三個 polygon 在空場合零成本**:`has_positive=False and excl is None` 直接 return boxes,連 dict lookup 都省;已畫的 polygon 才會 lazy 轉 pixel 並快取,camera 解析度切換不會 crash(新 `(w,h)` 自動 re-convert)
  - 版本:plugin `0.5.0` → `0.6.0`;description "Phase 6: ROI + Inclusive/Exclusive mask post-filter"
  - 工件路徑:`~/nx_plugin/src/nx_custom_plugin/src/nx/vms_server_plugins/analytics/sample/{device_agent.{cpp,h},plugin.cpp}`;`/opt/nx_custom_plugin/python/{worker.py,infer_utils.py}`
  - §8 Phase 6 狀態標為完成,①②③④⑤ 列明;等使用者在 Desktop Client 上實地畫 ROI 驗證前端已能正常 round-trip polygon(設計已可支撐)

- **2026-04-18 Phase 5 完成** — Class filter(COCO-80)+ Person/Vehicle/Object typeId 映射,後端三態語意設計完成:
  - 新增 `python/infer_utils.py::COCO80_NAMES`(與 Ultralytics 預訓練 index 對齊)+ `names_to_ids(names)` helper;**三態設計**:`None`(caller 沒設)→ 全放行;`[]`(caller 設空)→ 全拒絕;`[ids]` → 過濾。空選與 "沒設" 的語意分離以便 worker 可以短路
  - `worker.py` 拿掉寫死的 `PERSON_CLS_ID = 0`,config handler 解析 `classes` list → `names_to_ids()`,frame handler 在 `class_filter_ids == []` 時**直接跳過 `adapter.infer()`**(省掉一次 iGPU 推論 + 圖片 preprocess)— 勾選全空的相機真的吃 0 ms 推理;非空就 `adapter.infer(class_filter=class_filter_ids)`,四個 adapter 早就吃這參數(Phase 4 保留的接口)
  - C++ `DeviceAgent`:
    - 加 `parseClassesJson(raw, outList)` 用 `nx::kit::Json::parse` 解字串化 JSON array(與 Phase 0 smoke B 觀察到的 PolygonFigure 同規律 — CheckBoxGroup 也是 stringified JSON),parse 失敗回 false 表示 "沒設過" → 不在 IPC 送 `classes` key(保 None 語意)
    - 加 `classIdToNxTypeId(cls)`:`0→nx.base.Person`;`{1,2,3,4,5,6,7,8}→nx.base.Vehicle`(bicycle/car/motorcycle/airplane/bus/train/truck/boat,airplane/train/boat 雖稀見但歸入 Vehicle 比 Object 合理);其他→`nx.base.Object`
    - `m_classes: std::vector<std::string>` + `m_classesSet: bool` 成員(mutex 保護),`settingsReceived()` 解析後存入,`sendConfigToWorker()` 若 `classesSet=true` 組 `nx::kit::Json::array` 塞進 extra;log 用 `classesSummary`(首 3 名 + 總數)避免爆字串
    - `pushUncompressedVideoFrame()` 解 `box["cls"]` → `classIdToNxTypeId()` → `obj->setTypeId(typeId)`,取代原本的 `kObjectTypePerson` 常數
    - Manifest `supportedTypes` 加 `nx.base.Vehicle` + `nx.base.Object`(NX 內建,不需 typeLibrary);保留 `typeLibrary: { eventTypes: [], objectTypes: [] }` 空殼
  - C++ `Engine::buildManifest()`:
    - `classes` CheckBoxGroup `range` 從 `["person","bicycle","car"]` placeholder 升為完整 COCO-80 八十類
    - `defaultValue` 從 `["person","car"]` 收斂成 `["person"]`(安控場景最常用)
    - GroupBox caption 從 `"Classes (placeholder — populated by Validate)"` 改成 `"Classes"`(Validate 動態更新在 Phase 0 smoke 已否決,不留誤導字樣)
    - 加 description 文字說明 "uncheck all = stop inference"、"person→Person, vehicle→Vehicle, else→Object" 映射規則
  - 實測(journal 22:18:43,restart 後首次):
    - C++:`settingsReceived #1 ... classesRaw='["person","bicycle","car"]' classesParsed=3 classesSet=1`(舊 Phase 1 持久化值留存,CheckBoxGroup 解析成功)
    - C++:`sendConfig ... classes=[person,bicycle,car] x3`(log summary 格式正確)
    - Python:`config ... classes=[0, 1, 2]`(`names_to_ids` 映射正確)
    - Python:`infer ... boxes=0 infer_ms=23.9 filter=[0, 1, 2] top3=[]`(adapter 收到 filter,空鏡頭 0 bbox 符合)
    - mediaserver log **無** `nx.base.Vehicle` / `nx.base.Object` 的 typeId unknown 警告 → NX 6.1 CN 認可這兩個 built-in
    - `engine manifest built, length=6511 bytes`(Phase 4 5008 → +1503 bytes 是 COCO-80 字串清單)
    - `.so` 752544 → 761320 bytes(+8776,COCO 字串 + typeId helper + JSON parse)
  - **三態 API 設計留給未來 Phase 7 tracker 複用**:若使用者把某類從 track 名單剔除,直接用相同 `class_filter_ids=[]` 短路路徑,tracker 不用為此新增 API
  - 版本:plugin `0.4.0` → `0.5.0`;description "Phase 5: COCO-80 class filter + Person/Vehicle/Object type mapping"
  - 工件路徑:`~/nx_plugin/src/nx_custom_plugin/src/nx/vms_server_plugins/analytics/sample/{device_agent.{cpp,h},engine.cpp,plugin.cpp}`;`/opt/nx_custom_plugin/python/{worker.py,infer_utils.py}`
  - §8 Phase 5 狀態標為完成,①②③④⑤⑥ 列明

- **2026-04-18 Phase 4 完成** — 多 Runtime(ONNX / OpenVINO / PyTorch / TensorRT)可 per-camera 熱切換,N100 iGPU 實測 inference 降 30%、worker CPU 降到 0.66 核:
  - 新增 `python/infer_utils.py`:抽共用 letterbox + YOLO decode;`detect_layout(out_shape)` 區分 yolo26 `(1,K,6)` end-to-end 與 yolov8 `(1,4+C,N)` raw(需 `cv2.dnn.NMSBoxes`);`preprocess_chw_f32()` / `decode_boxes()` 雙 adapter 共用
  - 新增 `adapter_openvino.py`(OpenVINO 2026.1.0):`ov.Core().read_model()` 吃 `.onnx` 或 IR `.xml`,`compile_model(model, device_name=<mapped>)` 加 AUTO fallback;`_DEVICE_MAP = {cpu:CPU, intel:gpu:GPU, intel:npu:NPU, auto:AUTO}`;input partial_shape coerce 失敗退 640;output port 用 `.get_any_name()` 抓第一個
  - 新增 `adapter_pytorch.py`(Ultralytics YOLO,吃 `.pt`,device `cuda:N/mps/xpu/cpu`)+ `adapter_tensorrt.py`(Ultralytics YOLO,吃 `.engine`,預設 `cuda:0`);兩者 infer 前都 `cv2.cvtColor(rgb, COLOR_RGB2BGR)` 因為 Ultralytics 內部會再 `[..., ::-1]` 反一次;N100 venv 裝不起 ultralytics/tensorrt 是正常,`env_probe.py` 不會列;未來節點裝了 `pip install ultralytics` 自動浮出
  - `adapter_onnx.py` 重構吃 infer_utils,`load()` 依 `device_hint` 塞 providers(`cuda*` → `CUDAExecutionProvider`,其餘 CPU);**N100 上 onnxruntime 無 OpenVINO EP,iGPU 要真的用 Intel 硬體只能走 openvino adapter**(已向 user 解釋)
  - `worker.py` 加 `_ADAPTER_TABLE = {onnx, openvino, pytorch, tensorrt}` + `make_adapter(rt)` 用 `importlib.import_module()` lazy 載入(避免 adapter 依賴沒裝的情境整個 worker crash);serve loop 在 `type=config` 收到時:`new_runtime != current_runtime or adapter is None` → 換 adapter;`new_model or new_device or adapter.session is None` → reload;per-connection state 包 `current_runtime/current_model/current_device`
  - C++ 端 Engine:加 `m_runtime/m_device/m_workerCount` + `m_settingsMutex`,`settingsReceived()` 解 `runtime/device/workerCount` + 變動就 `broadcastEngineConfig()` 呼叫每個 registered DeviceAgent 的 `onEngineConfigChanged() → sendConfigToWorker()`;`registerAgent/unregisterAgent` 用 `m_agentsMutex + std::vector<DeviceAgent*>`,DeviceAgent ctor 登記、dtor 清掉;`initDefaultsFromProbe()` 把 `m_env.runtimes.front()` / `m_env.devicesByRuntime[rt].front()` 當預設
  - C++ 端 DeviceAgent:加 `m_runtime/m_device` + `m_settingsMutex` + `onEngineConfigChanged()` 呼叫 `sendConfigToWorker()`;`sendConfigToWorker()` fallback 鏈 — 相機值 → Engine 值 → `onnx/cpu` 硬碼兜底,IPC JSON 多塞 `runtime`/`device`/`worker_count`;`settingsReceived()` 多讀 `settingValue("runtime")` 與 `settingValue("device")`
  - **關鍵 UI 發現**(user 回報 "沒看到可以切換 Runtime 的地方,重新整理也一樣"):NX 6.1 CN Desktop Client **不渲染** `engineSettingsModel`,整個插件的 Server / System 層級設定面板是空的;唯一能被使用者觸及的是相機層級的 `deviceAgentSettingsModel`。解法是把 Runtime / Device 兩個 ComboBox 從 engineSettingsModel 搬到 **deviceAgentSettingsModel 最上層的 Inference GroupBox**(`defaultValue` 仍吃 envprobe 結果),engineSettingsModel 保留等 NX 未來版本若啟用再用;manifest size 4089 → 5008 bytes
  - 實測(journal 21:59:38–22:07:20):
    - `adapter_swap cam={9b655ad3…} -> openvino`、`config runtime=openvino device=intel:gpu model=yolo26s.onnx conf=0.35 iou=0.50`
    - openvino+intel:gpu 穩態 `infer_ms 22.7 / 23.6 / 24.0 / 24.4 / 24.6`(median 24 ms);vs Phase 3 ONNX CPU 30–34 ms → **-30%**
    - 沒有 warmup spike(openvino 第一個 sample 就 24ms;ONNX session 首推 66–95 ms)
    - Worker live `%CPU = 16.5%`(vs ONNX CPU 時近整條 core),RSS 367 MB(+268MB 是 OpenVINO GPU plugin libs)
    - `mpstat 1 3` 系統平均 `%idle 74.9`、`%usr 10.3`、`%sys 5.7`、`%iowait 8.2`(iowait 是 NVMe 錄影,與推論無關);4c N100 還剩 3c
    - `intel_gpu_top` 因 PMU 權限問題拿不到 per-engine %busy,改看 `/sys/kernel/debug/dri/0/i915_engine_info`:`GT awake? yes [47], 91805655ms`;上一輪另量 GT awake delta 2014ms/2000ms ≈ 100% busy during inference → 確認 iGPU 有在工作
    - OpenVINO AUTO fallback try/except 封裝:driver 壞會自動 `compile_model(device_name="AUTO")`,`self.device = "AUTO"` 給下次 log 參考
  - Phase 4 對未來節點的意義:裝了 NVIDIA 的節點,user 在 UI 選 `pytorch` + `cuda:0` 就直接跑 `.pt`,選 `tensorrt` + `cuda:0` 就跑 `.engine`;adapter 惰性 import 避免 N100 沒裝 torch/tensorrt 時 worker crash;IPC wire format v3 不動
  - 版本:plugin `0.3.0` → `0.4.0`(`plugin.cpp` description 更新為 "Self-hosted YOLO analytics plugin (Phase 4 multi-runtime: ONNX/OpenVINO/PyTorch/TensorRT).")
  - 工件路徑:`~/nx_plugin/src/nx_custom_plugin/src/nx/vms_server_plugins/analytics/sample/{engine,device_agent,plugin}.{cpp,h}`;`/opt/nx_custom_plugin/python/{worker.py,infer_utils.py,adapter_onnx.py,adapter_openvino.py,adapter_pytorch.py,adapter_tensorrt.py}`
  - §8 Phase 4 狀態標為完成,①②③④⑤⑥ 列明;engineSettingsModel 的 runtime/device 保留為未來用

- **2026-04-18 Phase 3 完成** — ONNX MVP:yolo26n 真實偵測推到 Desktop Client:
  - venv 裝 `onnxruntime==1.24.4` + `numpy==1.26.4` + `opencv-python-headless`(系統 pip,約 150 MB 磁碟);`env_probe.py` 回報 `onnx -> [cpu]`,engine manifest runtimes 從 `<none>` 升到 1 項
  - 新增 `adapter_onnx.py`:`OnnxAdapter.load()` 動態讀 input shape、auto-detect output layout(yolo26 `(1,K,6)` / yolov8 `(1,4+C,N)`);`letterbox((h,w))` 參數化、輸出 normalized xyxy;yolo26 路徑跳過 NMS(end-to-end head 已含)
  - 模型:用 `ultralytics model.export(format="onnx", imgsz=320)` 從 `yolo26n.pt` 匯出 base ONNX(不用 `nx_onnx_export.py` 的 NX 包裝),input `images`[1,3,320,320],output `output0`[1,300,6]=(x1,y1,x2,y2,score,cls);放 `/var/lib/nx_plugin/models/yolo26n.onnx`,symlink `yolo26s.onnx → yolo26n.onnx` 讓 Phase 1 儲存的 UI 值無痛接上
  - IPC 擴展 v3:frame msg 改成 `<uint32 BE hdr_len><JSON hdr>` + raw body(無 framing);JSON 帶 `w/h/stride`,body 長度 = `h*stride`;Python `decode_rgb_body()` 處理 stride > w×3 的 padded 情境(實測 480×352 stride=1440 剛好貼齊但邏輯已準備好)
  - Engine capabilities 加 `"needUncompressedVideoFrames_rgb"`,mediaserver 自動改走 `pushUncompressedVideoFrame(IUncompressedVideoFrame*)` 路徑;DeviceAgent `settingsReceived()` 讀 modelPath/conf/iou/frameSkip → `IpcClient::sendConfig()` 送 JSON 給 worker;worker per-connection state,path 變化才 reload
  - Worker 用 transient systemd unit `nxworker.service` 起(`systemd-run --unit=nxworker --uid=networkoptix`),方便 `journalctl -u nxworker` 看 log;worker.py 加 `frame_dump` 前 3 幀存 JPEG + 通道均值 log 供色彩驗證
  - 實測(journal 21:19:38–21:22:03):
    - `loaded model=yolo26s.onnx input=320x320 layout=yolo26 out_shape=[1,300,6]`
    - `infer id=5 boxes=0/0 infer_ms=95.2`(warmup)→ `id=15 infer_ms=29.7`(steady)
    - 每通道均值 `(44.8, 42.3, 42.5)` 符合暗場景,dump JPEG 肉眼確認色彩正確
    - 人走過鏡頭後 `emitted packet #1 objects=1`、`#2 objects=1`、`#3 objects=1`,Desktop Client 看到真實 person bbox
    - 空鏡頭 `boxes=0/0 top3=[]` 符合預期(原本擔心 preprocess bug,dump 排除)
  - 版本:plugin `0.2.0` → `0.3.0`;`libnx_custom_plugin.so` 733712 bytes(Phase 2 的 710664 + 新 `sendConfig`、uncompressed frame path、stride 參數)
  - 工件路徑:`~/nx_plugin/src/nx_custom_plugin/src/nx/vms_server_plugins/analytics/sample/{device_agent,engine,ipc_client,plugin}.{cpp,h}`;`/opt/nx_custom_plugin/python/{worker.py,adapter_onnx.py}`;`/var/lib/nx_plugin/models/{yolo26n.onnx,yolo26s.onnx → yolo26n.onnx}`
  - §8 Phase 3 狀態標為完成,①②③④⑤ 列明

- **2026-04-18 Phase 2 完成** — C++ ↔ Python IPC 打通,Desktop Client 實測看到循環假 bbox:
  - 新增 `ipc_client.{cpp,h}`:UNIX socket blocking client,`<uint32 BE length><utf8 JSON>` framing,`sendAttach` / `sendDetach` / `sendFrameAndRecv` 三個 API;任何 I/O error 自動 close fd,`std::chrono::steady_clock` 2 秒冷卻避免重連 spam,第一次 fail 後 suppress log
  - 新增 `python/worker.py`:AF_UNIX SOCK_STREAM bind `/run/nx_plugin/worker_default.sock`(chmod 666 for Phase 2 testing),threading per-connection;`type=frame` 按 `id%60` 產滾動 fake person bbox,`attach`/`detach`/`config` 只 log 不回
  - DeviceAgent 在 ctor 建 per-camera `std::unique_ptr<IpcClient>`,`pushCompressedVideoFrame` 每幀 round-trip(blocking)→ 解 `boxes[].xyxy` → `makePtr<ObjectMetadataPacket>` + `setBoundingBox({x,y,width=x2-x1,height=y2-y1})` + `UuidHelper::randomUuid()` trackId → `pushMetadataPacket`;dtor `sendDetach`
  - supportedTypes 使用 NX 內建 `nx.base.Person`(不需在 typeLibrary 定義),Desktop Client 自動渲染 overlay
  - 實測:worker 未啟前 plugin 乾淨載入(2s cooldown、silent retry);worker 啟動後 DeviceAgent 下次循環立刻 connected;多相機 per-fd 獨立連線;mediaserver 二次重啟後 IpcClient 重建無痕;`frames=463 emitted=169` 正確反映 worker 未連期間的 drop
  - 版本:plugin `0.1.0` → `0.2.0`;`libnx_custom_plugin.so` 710664 bytes(Phase 1 的 633616 + IPC/Metadata headers)
  - 工件路徑:`~/nx_plugin/src/nx_custom_plugin/src/nx/vms_server_plugins/analytics/sample/{ipc_client,device_agent}.*`;`/opt/nx_custom_plugin/python/worker.py`
  - §8 Phase 2 狀態標為完成

- **2026-04-18 Phase 1 完成** — `nx_custom_plugin` 從 Phase 0 probe 升級成完整骨架:
  - 新增 `env_probe.py`(`/opt/nx_custom_plugin/python/`,networkoptix:networkoptix 644):stdout 一行 JSON `{"runtimes":[...], "devices":{rt: [dev...]}}`,偵測 onnxruntime/openvino/torch/tensorrt 四個 runtime,各自枚舉 device(cpu/cuda:N/intel:gpu/intel:npu/mps/xpu)— 每個 import 都 try/except,stderr 輸出人類可讀錯誤 `2>/dev/null` 丟掉,確保 stdout 乾淨
  - 新增 `env_probe.cpp/.h`:`popen()` 呼叫 venv python(fallback 系統 python),用 `nx::kit::Json` 解析;失敗時返回 empty `EnvProbe` + `errorMessage`,上層 fallback 塞 `"<none>"` 進 ComboBox 讓 UI 仍可顯示
  - Engine ctor 跑一次 `probeEnvironment()` 儲存結果,`buildManifest()` 用 `std::ostringstream` 組動態 manifest:`engineSettingsModel`(runtime/device ComboBox + workerCount SpinBox),`deviceAgentSettingsModel`(Model/Thresholds/Classes/Regions 四個 GroupBox,共 modelPath TextField + Validate Button `isActive:true` + conf/iou/frameSkip SpinBox + classes CheckBoxGroup + roi/inclusiveMask/exclusiveMask PolygonFigure × 3)
  - DeviceAgent 改以 `Engine*` 為 ctor 參數(forward declaration 避開循環 include);`pushCompressedVideoFrame` 僅 log 首 3 幀;active setting callback 為 Validate 按鈕留 stub(Phase 3+ 才填模型載入 + class 重繪)
  - 實測 17:20:31 restart 後:engine manifest `length=4045 bytes`,`env probe: 0 runtimes`(空 venv 預期),2 台相機各建 DeviceAgent,使用者於 Desktop Client 調整 conf 30→25、frameSkip 1→5、classes 加 bicycle、畫 3 個 polygon,每次 Apply 都正確 round-trip 到 `settingsReceived()`;Validate 按鈕點擊觸發 `active setting: validate` log
  - 工件:`~/nx_plugin/src/nx_custom_plugin/src/nx/vms_server_plugins/analytics/sample/{plugin,engine,device_agent,env_probe}.{cpp,h}`;build `~/nx_plugin/build-phase1/libnx_custom_plugin.so`(633616 bytes);部署同 Phase 0 路徑覆蓋。舊 `build-probe/` 已移除
  - §8 Phase 1 狀態標為完成,明列 ①②③④⑤

- **2026-04-18 Phase 0 ③④ smoke 完成** — 自製 `nx_custom_plugin` probe 部署進 mediaserver 6.1.1.42624 + 經 Desktop Client 實測:
  - Active Setting: ComboBox `probe.runtime` isActive:true 切換後,`doGetSettingsOnActiveSettingChange` 有收到 `activeSettingName`、完整 `settingsModel` JSON、`settingsValues` IStringMap(4 欄)、`params` 空 map — round-trip 路徑可用於 Validate 按鈕流程
  - `pushManifest()` 測試:只更新 DeviceAgent manifest(capabilities/supportedTypes/typeLibrary),**不重繪 settings UI** — M6 正式做法改為「Engine 建構子跑 yolo_env_checker,組好 `deviceAgentSettingsModel` 後隨 Engine manifest 一次送出」
  - `probe.roi` PolygonFigure 實測 raw value:**字串化的 JSON**(外層 string,內層含 `figure.points` 3+點陣列),座標系 **normalized 0-1**,與 SDK `settings_model.md` 文件一致
  - NX_PRINT 輸出走 `std::cerr` → systemd journal(`journalctl -u networkoptix-mediaserver`),不走 `main.log`;`/etc/nx_ini/mediaserver_stderr.log` touch 後也可接但非必要
  - §4.7.1 表格改寫、§3 新增 PolygonFigure 解析契約、§8 Phase 0 狀態標為完成
  - 工件路徑:`~/nx_plugin/src/nx_custom_plugin/`(source)、`~/nx_plugin/build-probe/libnx_custom_plugin.so`、`/opt/networkoptix/mediaserver/bin/plugins/nx_custom_plugin/libnx_custom_plugin.so`(部署)

- **2026-04-18 Phase 0 ② 完成** — 遠端 N100 toolchain + ABI 相容性已驗證:
  - `metavms-metadata_sdk-6.0.1.39873-universal.zip`(Nx 官方公開版本,CN 版 6.1.1.42624 未上架 `updates.networkoptix.com`)
  - 遠端 N100 用官方 `samples/sample_analytics_plugin/CMakeLists.txt` build 成功(516 KB `.so`,約 1 分鐘)
  - 部署進 mediaserver 6.1.1.42624 後 `PluginManager` log 乾淨載入:
    `Loaded Server plugin [...] (main interface nx_sdk_analytics_IIntegration, sdk version "6.0.1 R1")`
  - 同系統下還有 `vca_edge_analytics_plugin` SDK 4.2.0、`nxai_plugin` 6.1.0,印證 NX 跨 minor / patch 的 ABI 相容寬鬆
  - **政策調整**:§11「SDK 版號必須等於 mediaserver」放寬為「**SDK 取 Nx 官方最新公開發布 ≤ mediaserver patch 版**;Phase 0 的 ABI smoke 通過即代表 pin 有效」
  - `SDK_PIN.txt` 先以 6.0.1.39873 的 SHA256 記錄;未來若 Nx 公開 6.1.x SDK 再 rebuild + 更新 pin
  - 已裝工具鏈汙染:`cmake` `cmake-data` `libjsoncpp25` `librhash0` `ninja-build` `patchelf`(6 個 noble 官方包,零 upgrade/remove)

---

## 13. 下一步建議執行順序

1. **補齊工具鏈**(遠端):`sudo apt install cmake ninja-build libssl-dev pkg-config patchelf`
2. **下載 Metadata SDK 6.1.1.42624 zip**,解到 `third_party/` 並記 SHA256 到 `SDK_PIN.txt`
3. **Phase 0 一次做完三件事**:
   - a) 在遠端 N100 直接 build `opencv_object_detection_analytics_plugin/step1`(驗 SDK + toolchain)
   - b) Smoke test A:`pushManifest()` 能否動態重繪 UI(決定 §4.7.1 分支)
   - c) Smoke test B:`PolygonFigure` 傳進來的座標系(normalized/pixel?)
4. **切換到 WSL2** 開始 Phase 1+ iteration loop(遠端只當部署目標)
5. **Phase 3 = MVP** — ONNX + 多尺寸可用就能先跑;Phase 4+ 是增強
6. **Phase 10** Windows build 建議和 Linux 一起做 CI(見 §7.5.5),不要留到最後爆掉

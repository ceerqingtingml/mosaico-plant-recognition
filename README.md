# 植物识别

基于 ESP-Mosaico Vibe 的 GSP 应用：首页、取景、Wi-Fi、识别结果和相册。

## 当前能力

- **Wi-Fi**：设备上扫描附近真实热点并可连接
- **相机预览**：进入拍照页后打开 `mosaico_module_camera` 实时预览
- **音量**：结果页使用滑条调节
- **相册**：默认空；仅保存后的记录，左右滑动切换，侧滑返回

## 尚未接入

**千问 / 云端识别尚未接入。** 拍照后不会上传图片，也不会返回真实植物信息。
识别页会明确提示“识别服务未接通”，不再使用预设示例植物。

## 构建与安装

```sh
cd /mnt/hdd/esp/esp-mosaico-vibe
. /mnt/hdd/esp/esp-idf-mosaico/export.sh
python3 mosaico.py iris system-update --project projects/plant_recognition
```

空白设备先 `python3 mosaico.py recover`。

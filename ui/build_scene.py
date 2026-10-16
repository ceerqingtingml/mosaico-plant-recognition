# Build the 森语 480×480 GSP scene from the HTML prototype layout.
# Run: python3 build_scene.py
import json
import struct
import zlib
from pathlib import Path

NODES = []

# Prototype palette
CREAM = "#F8F6ED"
PAPER = "#FFFDF7"
FOREST = "#2D5144"
FOREST_DEEP = "#18352C"
SAGE = "#477161"
SAGE_SOFT = "#DCE7DD"
MUTED = "#708078"
SUN = "#EEBD68"
INK = "#23372F"
CAMERA_BG = "#14241E"

PRINTABLE = "".join(chr(c) for c in range(32, 127))
UI_EXTRA_GLYPHS = "本"
RESULT_EXTRA_GLYPHS = "菀"

DYNAMIC = [
    # Home / status
    "Wi-Fi 未连接", "未联网", "已连接", "今日宜 · 亲近自然", "去认识一株植物",
    "轻触开始", "拍照识花草", "对准花、叶或果实", "识别小贴士",
    "保持光线充足，主体清晰完整，结果会更准确。",
    "足迹", "叶",
    # Wi-Fi
    "初次见面", "连接一片云", "选择 Wi-Fi，让植物知识随风而来",
    "附近网络", "重新扫描", "返回",
    "正在寻找附近网络…", "请稍候", "未发现网络，可重新扫描",
    "扫描超时，请重试", "Wi-Fi 扫描启动失败", "连接成功", "正在连接…",
    "发现 0 个网络，点选连接", "发现 24 个网络，点选连接",
    "密码错误或无法连接", "连接失败，请重试",
    # Password
    "连接网络", "输入密码", "正在连接", "Wi-Fi 密码", "查看", "连接 Wi-Fi",
    "输入密码后点 OK 或连接", "连接",
    # Camera
    "拍摄植物", "让植物完整出现在框内", "相册", "拍照",
    "相机打开失败，请检查模组", "界面缺少预览画布", "请在设备上查看实时预览",
    "画面已旋转", "对准植物后点击拍照",
    # Loading / result
    "正在翻阅自然图鉴", "辨认这株植物…", "正在观察叶片、花瓣与生长形态",
    "识别中，请稍候", "正在准备识别…", "已拍照，正在请求识别服务…",
    "拍照/识别链路尚未接通", "无法开始识别", "未识别",
    "当前固件尚未接入千问识别服务，没有云端识别结果。",
    "接入识别 API 后可显示真实结果", "匹配度 96%", "匹配度 92%",
    "保持光线充足，主体清晰完整",
    "小百科", "诗意", "习性", "再识一株", "讲解", "保存",
    "正在语音讲解", "语音讲解启动失败", "已保存到相册", "保存照片",
    "照片默认不保存", "本次照片未保存",
    "识别服务未配置", "请在 menuconfig 中配置 Qwen API Key",
    "网络不可用，请先连接 Wi-Fi", "拍照失败，请重试", "识别失败，请稍后重试",
    "板载 NAND 不可用", "存储空间不足", "保存失败，请重试",
    "山茶花", "山茶科", "Camellia japonica L.",
    "THEACEAE · 山茶科",
    "山茶花常绿灌木或小乔木，花期多在冬春之际。",
    "唯有山茶偏耐久，绿丛又放数枝红。",
    # Gallery
    "相册是空的", "识别后点保存才会出现在这里", "相册 1/1", "相册 1/2",
    "删除", "照片读取失败", "相册", "点击照片查看",
    "确定删除这张照片？", "删除后无法恢复。", "取消",
    "1/2 上滑", "2/2 下滑",
    # Volume
    "70", "80", "60", "100", "0", "音量",
]


def write_solid_png(name, size, color):
    """Opaque PNG. A unique color keeps GSP from merging it with another canvas."""
    ui_dir = Path(__file__).resolve().parent

    def chunk(tag, data):
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    row = b"\x00" + bytes(color) * size
    raw = row * size
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    (ui_dir / name).write_bytes(png)


def write_thumb_placeholders(size):
    """Write six opaque PNGs that differ by color so GSP keeps them distinct."""
    for index in range(6):
        write_solid_png(f"thumb_placeholder_{index}.png", size,
                        (0xDC - index, 0xE7, 0xDD))



def add(parent, **fields):
    node = dict(fields)
    node["_parent"] = parent
    if "name" not in node:
        node["_id"] = f"id{len(NODES)}"
    NODES.append(node)
    return node["name"] if "name" in node else node["_id"]


def label(parent, text, x, y, w, h, size=18, color=FOREST, align="left", **extra):
    add(parent, type="label", text=text, x=x, y=y, w=w, h=h,
        font_size=size, fg_color=color, text_align=align, **extra)


def button(parent, name, target, text, x, y, w, h, bg, fg=CREAM, size=18, radius=16):
    add(parent, type="button", name=name, text=text, x=x, y=y, w=w, h=h,
        font_size=size, fg_color=fg, bg_color=bg, radius=radius,
        events=[{"event": "click", "action": "call", "target_name": target}])


def box(parent, x, y, w, h, color, radius=0, **extra):
    return add(parent, type="container", x=x, y=y, w=w, h=h,
               bg_color=color, radius=radius, **extra)


def charset(include_common=False):
    chars = set(PRINTABLE)
    chars.update("°·…—")
    common_path = Path(__file__).with_name("common_chinese_3500.txt")
    if include_common and common_path.exists():
        chars.update(ch for ch in common_path.read_text(encoding="utf-8")
                     if not ch.isspace())
    if include_common:
        # Recognition text is supplied by the cloud and cannot be enumerated
        # ahead of time. Add all 3,755 GB2312 level-1 Han characters to the
        # existing plant-focused set. Level 2 would make the generated bundle
        # exceed the device's UI partition.
        for lead in range(0xB0, 0xD8):
            for trail in range(0xA1, 0xFF):
                try:
                    chars.update(bytes((lead, trail)).decode("gb2312"))
                except UnicodeDecodeError:
                    pass
    for text in DYNAMIC:
        chars.update(text)
    # Extra glyphs used by live SSID / status formatting
    chars.update("国移动电信联通宽带线家公司酒店访客房办教宿舍园区智"
                 "河银翼小米华荣耀苹美海尔格力发扫描失败重试"
                 "一二三四五六七八九百千万下左右高低快慢新老个机打检查组")
    chars.update(UI_EXTRA_GLYPHS)
    if include_common:
        chars.update(RESULT_EXTRA_GLYPHS)
    return "".join(sorted(chars))


def write_charset_file(path: Path, text: str):
    path.write_text(text + "\n", encoding="utf-8")


def bound(name, charset_file="ui_charset.txt", **kwargs):
    kwargs["name"] = name
    kwargs["bind"] = name
    kwargs["font_charset_file"] = charset_file
    return kwargs


def page(parent, color):
    box(parent, 0, 0, 480, 480, color, 40)


def build():
    # ---- Home (prototype homePage) ----
    home = add(None, type="layer", x=0, y=0, w=480, h=480, name="home",
               bind="home_visible", bind_target="visible", hidden=False)
    page(home, CREAM)
    label(home, "今日宜 · 亲近自然", 32, 40, 280, 24, 14, SAGE)
    label(home, "去认识一株植物", 32, 68, 340, 40, 30, FOREST)
    label(home, " ", 32, 104, 280, 24, 16, MUTED, **bound("wifi_label"))
    button(home, "button_wifi", "open_wifi", "叶", 400, 40, 48, 48,
           SAGE_SOFT, FOREST, 20, 24)
    # Compact connection state: an unobtrusive dot on the Wi-Fi button.
    box(home, 434, 74, 12, 12, PAPER, 6)
    box(home, 436, 76, 8, 8, MUTED, 4,
        name="wifi_indicator", bind="wifi_indicator", bind_target="color")

    # Hero card sits just under the title; network state stays on the Wi-Fi page.
    box(home, 28, 132, 424, 202, SAGE, 36)
    label(home, "轻触开始", 52, 154, 200, 24, 14, SAGE_SOFT)
    label(home, "拍照识花草", 52, 184, 300, 40, 30, "#FFFFFF")
    label(home, "对准花、叶或果实", 52, 232, 280, 28, 16, SAGE_SOFT)
    button(home, "button_start", "start_scan", "开始", 52, 278, 120, 40,
           "#FFFFFF", FOREST, 18, 20)

    # Give the tip its own hierarchy instead of compressing it into one line.
    box(home, 28, 350, 424, 74, PAPER, 22)
    label(home, "识别小贴士", 48, 362, 160, 24, 15, FOREST)
    label(home, "保持光线充足，主体清晰完整。", 48, 388, 360, 22, 13, MUTED)

    # ---- Camera (prototype cameraPage) ----
    camera = add(None, type="layer", x=0, y=0, w=480, h=480, name="camera",
                 bind="camera_visible", bind_target="visible", hidden=True)
    page(camera, CAMERA_BG)
    # Canvas live preview: must be unscaled profile-native opaque + lossless.
    # fit/radius/dynamic_image break esp_gsp_canvas_try_push eligibility.
    add(camera, type="image", name="preview", bind="preview_surface",
        x=0, y=0, w=480, h=480, image="preview_placeholder.png",
        codec="lossless")
    # Top bar
    button(camera, "button_back_camera", "back_home", "返回", 16, 16, 72, 44,
           FOREST, "#FFFFFF", 16, 22)
    label(camera, "拍摄植物", 120, 22, 240, 32, 20, "#FFFFFF", "center")
    button(camera, "button_help", "toggle_flash", "闪光", 392, 16, 72, 44,
           FOREST, "#FFFFFF", 16, 22)
    # Focus hint (also used for camera error text)
    label(camera, "让植物完整出现在框内", 40, 220, 400, 32, 16, "#FFFFFF", "center",
          **bound("preview_note"))
    # Hidden binds kept for plant_ui compatibility
    label(camera, "闪光灯关", 0, 0, 1, 1, 10, CAMERA_BG, **bound("flash_label"))
    label(camera, "画面 0°", 0, 0, 1, 1, 10, CAMERA_BG, **bound("rotate_label"))
    # Bottom controls
    button(camera, "button_gallery", "open_gallery", "相册", 28, 400, 88, 56,
           FOREST, "#FFFFFF", 16, 18)
    button(camera, "button_capture", "capture", "拍照", 176, 388, 128, 80,
           "#FFFFFF", FOREST, 22, 40)
    button(camera, "button_flip", "toggle_rotate", "旋转", 364, 400, 88, 56,
           FOREST, "#FFFFFF", 16, 18)

    # ---- Wi-Fi (prototype wifiPage) ----
    wifi = add(None, type="layer", x=0, y=0, w=480, h=480, name="wifi",
               bind="wifi_visible", bind_target="visible", hidden=True)
    page(wifi, CREAM)
    label(wifi, "未连接", 280, 16, 168, 20, 12, MUTED, "right",
          **bound("wifi_connection"))
    label(wifi, "初次见面", 32, 36, 220, 24, 14, SAGE)
    label(wifi, "连接一片云", 32, 64, 300, 40, 30, FOREST)
    label(wifi, "选择 Wi-Fi，让植物知识随风而来", 32, 112, 320, 28, 14, MUTED,
          **bound("wifi_status"))
    button(wifi, "button_rescan", "rescan", "重新扫描", 360, 108, 96, 36,
           FOREST, "#FFFFFF", 14, 16)
    box(wifi, 24, 156, 432, 272, PAPER, 24)
    label(wifi, "附近网络", 40, 168, 160, 24, 14, MUTED)
    add(wifi, type="list", name="networks", x=36, y=198, w=408, h=214,
        item_height=56, item_count=0, row_template="networks_row",
        scroll_snapshot=True, bg_color=PAPER, fg_color=FOREST)
    row = add(None, type="container", name="networks_row_root",
              template="networks_row", callback="select_networks",
              x=0, y=0, w=408, h=56, bg_color=PAPER, radius=0)
    box(row, 0, 0, 408, 50, "#FFFFFF", 14)
    label(row, "Network", 16, 11, 330, 28, 18, FOREST, **bound("networks_title"))
    box(row, 372, 12, 12, 14, "#FFFFFF", 6, name="lock_shackle", dynamic_color=True)
    box(row, 375, 15, 6, 11, "#FFFFFF", 3)
    box(row, 369, 23, 18, 15, "#FFFFFF", 3, name="lock_body", dynamic_color=True)
    box(row, 377, 28, 2, 5, "#FFFFFF", 1)

    # ---- Password ----
    password = add(None, type="layer", x=0, y=0, w=480, h=480, name="password",
                   bind="password_visible", bind_target="visible", hidden=True)
    page(password, CREAM)
    button(password, "button_back_password", "password_back", "返回", 24, 28, 72, 44,
           SAGE_SOFT, FOREST, 16, 22)
    label(password, "连接网络", 88, 28, 200, 20, 12, SAGE)
    label(password, "输入密码", 88, 50, 280, 32, 24, FOREST)
    label(password, "连接 PlantLab", 32, 96, 416, 28, 16, FOREST,
          **bound("password_title"))
    box(password, 24, 132, 432, 52, "#FFFFFF", 16)
    label(password, "", 40, 144, 400, 28, 18, FOREST, input=True,
          name="password_input", bind="password_input",
          font_charset=PRINTABLE)
    label(password, "输入密码后点 OK 或连接", 28, 196, 300, 28, 14, MUTED,
          **bound("password_status"))
    button(password, "button_connect", "connect", "连接 Wi-Fi", 300, 192, 156, 40,
           FOREST, "#FFFFFF", 16, 18)
    add(password, type="keyboard", name="password_keyboard",
        x=16, y=244, w=448, h=220, font_size=16,
        bg_color=CREAM, fg_color=FOREST,
        key_color="#FFFFFF", function_color=SAGE_SOFT, function_text_color=FOREST,
        delete_color=SAGE_SOFT, delete_text_color=FOREST,
        space_color="#FFFFFF", space_text_color=MUTED,
        ok_color=FOREST, ok_text_color="#FFFFFF", key_radius=10)

    # ---- Result: the upper hero is the actual captured photograph. ----
    result = add(None, type="layer", x=0, y=0, w=480, h=480, name="result",
                 bind="result_visible", bind_target="visible", hidden=True)
    page(result, CAMERA_BG)
    # Own file: the live preview uses preview_placeholder.png. Sharing that
    # image makes canvas_stop on the preview restore this green plate too.
    write_solid_png("result_placeholder.png", 480, (0x14, 0x24, 0x1E))
    add(result, type="image", name="result_photo", bind="result_photo_surface",
        x=0, y=0, w=480, h=480, image="result_placeholder.png",
        codec="lossless")
    label(result, "匹配度 —", 160, 24, 160, 28, 14, "#FFFFFF", "center",
          **bound("result_confidence"))
    box(result, 18, 160, 444, 310, CREAM, 32, bg_opacity=245)
    label(result, "识别中，请稍候", 38, 180, 300, 36, 26, FOREST,
          **bound("result_name", "result_charset.txt"))
    label(result, " ", 38, 218, 380, 24, 15, MUTED,
          **bound("result_latin", "result_charset.txt"))
    label(result, " ", 38, 248, 404, 64, 15, MUTED,
          **bound("result_summary", "result_charset.txt"))
    label(result, " ", 38, 316, 404, 44, 15, FOREST,
          **bound("result_poem", "result_charset.txt"))
    label(result, " ", 38, 365, 270, 22, 13, MUTED, **bound("result_status"))
    label(result, "音量", 38, 401, 48, 24, 14, MUTED)
    label(result, "70", 244, 401, 44, 24, 14, FOREST, "right",
          **bound("volume_value"))
    add(result, type="slider", name="volume_slider", bind="volume_level",
        x=90, y=399, w=148, h=28, min=0, max=100, value=70,
        track_size=10, radius=14, bg_color=SAGE_SOFT, fg_color=FOREST,
        knob_color="#FFFFFF")
    button(result, "button_speak", "speak", "讲解", 308, 386, 64, 64,
           SUN, FOREST, 15, 32)
    button(result, "button_save", "save_photo", "保存", 382, 396, 64, 44,
           FOREST, "#FFFFFF", 15, 22)

    # ---- Gallery: thumbnail grid, then swipeable full-screen detail. ----
    gallery = add(None, type="layer", x=0, y=0, w=480, h=480, name="gallery",
                  bind="gallery_visible", bind_target="visible", hidden=True)
    page(gallery, CREAM)
    grid = add(gallery, type="layer", name="gallery_grid",
               bind="gallery_grid_visible", bind_target="visible",
               x=0, y=0, w=480, h=480)
    label(grid, "相册", 28, 28, 260, 36, 28, FOREST)
    label(grid, "点击照片查看", 28, 64, 424, 24, 14, MUTED,
          **bound("gallery_page"))
    label(grid, "识别后点保存才会出现在这里", 32, 220, 416, 32, 15,
          MUTED, "center", **bound("gallery_empty_status"))
    # 128 keeps RGB565 stride (256 bytes) on a 64-byte DMA boundary.
    # 136 does not, and the canvas copy then leaves a black band.
    # Each thumbnail also needs its own file: GSP deduplicates identical
    # images, and one shared canvas makes every cell show the last photo.
    thumb_size = 128
    write_thumb_placeholders(thumb_size)
    thumb_positions = [(30, 108), (176, 108), (322, 108),
                       (30, 256), (176, 256), (322, 256)]
    for index, (x, y) in enumerate(thumb_positions):
        thumb = add(grid, type="container", name=f"thumb_{index}",
                    bind=f"thumb_{index}_visible", bind_target="visible",
                    x=x, y=y, w=thumb_size, h=thumb_size,
                    bg_color=SAGE_SOFT, radius=18, hidden=True)
        add(thumb, type="image", name=f"thumb_{index}_image",
            bind=f"thumb_{index}_surface", x=0, y=0,
            w=thumb_size, h=thumb_size,
            image=f"thumb_placeholder_{index}.png", codec="lossless")

    detail = add(gallery, type="layer", name="gallery_detail",
                 bind="gallery_detail_visible", bind_target="visible",
                 x=0, y=0, w=480, h=480, hidden=True)
    write_solid_png("gallery_placeholder.png", 480, (0x18, 0x35, 0x2C))
    add(detail, type="image", name="gallery_photo", bind="gallery_photo_surface",
        x=0, y=0, w=480, h=480, image="gallery_placeholder.png",
        codec="lossless")
    box(detail, 16, 16, 448, 56, FOREST_DEEP, 24, bg_opacity=210)
    label(detail, "相册是空的", 32, 28, 300, 32, 20, "#FFFFFF",
          **bound("gallery_title"))
    label(detail, "识别后点保存才会出现在这里", 32, 424, 340, 32, 14, "#FFFFFF",
          **bound("gallery_caption"))
    button(detail, "button_delete", "gallery_delete", "删除", 376, 22, 72, 44,
           "#9B3E36", "#FFFFFF", 15, 22)

    # Centered confirm card. The old caption sat on the photo in white.
    dialog = add(gallery, type="layer", name="gallery_delete_dialog",
                 bind="gallery_delete_visible", bind_target="visible",
                 x=0, y=0, w=480, h=480, hidden=True)
    box(dialog, 0, 0, 480, 480, "#18352C", 0, bg_opacity=150)
    box(dialog, 40, 140, 400, 200, PAPER, 28)
    label(dialog, "确定删除这张照片？", 64, 164, 352, 36, 22, FOREST, "center")
    label(dialog, "删除后无法恢复。", 64, 208, 352, 28, 15, MUTED, "center")
    button(dialog, "button_delete_cancel", "gallery_delete_cancel", "取消",
           64, 260, 160, 52, SAGE_SOFT, FOREST, 18, 26)
    button(dialog, "button_delete_confirm", "gallery_delete_confirm", "删除",
           256, 260, 160, 52, "#9B3E36", "#FFFFFF", 18, 26)

    # Resolve parent indices
    names = {}
    for index, node in enumerate(NODES):
        key = node.get("name", node.get("_id"))
        names[key] = index
    objects = []
    for node in NODES:
        parent = node.pop("_parent")
        node.pop("_id", None)
        node["parent"] = -1 if parent is None else names[parent]
        objects.append(node)

    charset_text = charset()
    result_charset_text = charset(include_common=True)
    write_charset_file(Path(__file__).with_name("ui_charset.txt"), charset_text)
    write_charset_file(Path(__file__).with_name("ssid_charset.txt"), charset_text)
    write_charset_file(Path(__file__).with_name("result_charset.txt"),
                       result_charset_text)

    scene = {
        "screen": "plant",
        "w": 480,
        "h": 480,
        "screen_bg": "#000000",
        "font": "fonts/NotoSansSC-Regular.otf",
        "default_font_size": 18,
        "swipe": False,
        "objects": objects,
    }
    out = Path(__file__).with_name("main.json")
    out.write_text(json.dumps(scene, ensure_ascii=False, indent=2) + "\n",
                   encoding="utf-8")
    print(f"wrote {out} objects={len(objects)} charset={len(charset_text)}")


if __name__ == "__main__":
    build()

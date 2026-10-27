import serial
import pyautogui
import json
import urllib.request
import urllib.parse
import re
import urllib.parse
import threading
import time
import asyncio
import io
import os
import re
import webbrowser
import ctypes
import xml.etree.ElementTree as ET

from ctypes import wintypes, POINTER, byref, c_int
from PIL import Image, ImageChops

import comtypes
from comtypes import GUID, IUnknown, COMMETHOD, HRESULT

from winsdk.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
)
from winsdk.windows.ui.notifications.management import UserNotificationListener, UserNotificationListenerAccessStatus
from winsdk.windows.ui.notifications import NotificationKinds

# =====================================================================
#  설정
# =====================================================================
PORT = os.environ.get("DECKLINK_PORT", "COM3")  # Pico USB serial port
LHM_URL = 'http://localhost:8085/data.json'

PLEX_URL = os.environ.get("PLEX_URL", "").rstrip("/")
PLEX_TOKEN = os.environ.get("PLEX_TOKEN", "")

LYRIC_OFFSET = 0.0
IDLE_OFF_SEC = 3600          # 이 시간 동안 입력·재생 없으면 화면 끔

ART_W = ART_H = 100
ICON_W = ICON_H = 48
BTN_BG = (0x2A, 0x2A, 0x2A)          # 보드 BTN_BG_HEX와 동일

NIGHT_OFF_HOUR = 20          # 이 시각 이후 화면 끔
NIGHT_ON_HOUR = 8            # 이 시각부터 다시 허용

NIGHT_WAKE_SEC = 60          # 밤에 터치하면 이만큼 켜둠

last_touch = 0.0
lyrics = []
last_lyric = None
src_last = None
art_sent = False
art_try = 0

# 페이지 1: 웹 즐겨찾기 (이름, URL) 6개
FAVORITES = [
    ('Google',   'https://www.google.com'),
    ('YouTube',  'https://www.youtube.com'),
    ('Naver',    'https://www.naver.com'),
    ('Gmail', 'https://mail.google.com', 'https://www.gstatic.com/images/branding/product/2x/gmail_2020q4_64dp.png'),
    ('GitHub',  'https://github.com'),
    ('Wikipedia', 'https://www.wikipedia.org'),
]

# 페이지 3: 시작메뉴 바로가기 검색 키워드
PAGE3_APPS = [
    ('Word',        ['word']),
    ('PowerPoint',  ['powerpoint']),
    ('Excel',       ['excel']),
    ('한글',        ['한글', 'hwp']),
    ('알PDF',       ['알pdf', 'alpdf']),
    ('메일플러그',  ['메일플러그', 'mailplug']),
]

SENSORS = {
    'cpu_load': '/intelcpu/0/load/0',
    'cpu_temp': '/intelcpu/0/temperature/8',
    'gpu_load': '/gpu-nvidia/0/load/0',
    'gpu_temp': '/gpu-nvidia/0/temperature/0',
    'ram_load': '/ram/load/0',
    'ram_used': '/ram/data/0',
    'ram_avail': '/ram/data/1',
}

TASKBAR_PIN_DIR = os.path.expandvars(
    r'%APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar')
START_DIRS = [
    os.path.expandvars(r'%ProgramData%\Microsoft\Windows\Start Menu\Programs'),
    os.path.expandvars(r'%APPDATA%\Microsoft\Windows\Start Menu\Programs'),
]
SKIP_TASKBAR_IDS = {
    'StartButton', 'SearchButton', 'SearchBoxButton', 'TaskViewButton',
    'WidgetsButton', 'ChatButton', 'CopilotButton',
}
NAME_ALIASES = {
    '파일 탐색기': [r'C:\Windows\explorer.exe'],
    'file explorer': [r'C:\Windows\explorer.exe'],
    'phone link': ['phone link', '휴대폰과 연결', 'shell:AppsFolder\\Microsoft.YourPhone_8wekyb3d8bbwe!App'],
    '휴대폰과 연결': ['phone link', '휴대폰과 연결', 'shell:AppsFolder\\Microsoft.YourPhone_8wekyb3d8bbwe!App'],
}

COMP_RE = re.compile(r'차트|chart|top ?100|playlist|플레이리스트|주간|월간|various', re.I)

CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cache')
os.makedirs(CACHE_DIR, exist_ok=True)

def cache_key(artist, title):
    return norm(artist)[:30] + '_' + norm(title)[:50]

# =====================================================================
#  시리얼
# =====================================================================
s = None
reconnected = False          # 재연결 직후 곡/아트 다시 보내라는 신호


def open_port():
    global s, reconnected
    while True:
        try:
            if PORT.startswith('socket://'):
                # Wi-Fi 경로: PORT = 'socket://deck.local:5555'
                s = serial.serial_for_url(PORT, timeout=1, write_timeout=3)
            else:
                s = serial.Serial(timeout=1, write_timeout=3)
                s.port = PORT
                s.baudrate = 115200
                s.dtr = False          # ESP32-S3 USB-JTAG: DTR/RTS 토글 시 리셋되므로 끈 채로 연다
                s.rts = False
                s.open()
            print('connected', PORT)
            reconnected = True
            return
        except serial.SerialException:
            s = None
            time.sleep(2)


def close_port():
    global s
    try:
        if s: s.close()
    except Exception:
        pass
    s = None


open_port()
print('listening on', PORT)

tx_lock = threading.Lock()


def tx(data: bytes):
    global s
    with tx_lock:
        if s is None:
            return
        try:
            s.write(data)
            s.flush()
        except serial.SerialException:
            print('port lost')
            close_port()


def to_rgb565_bytes(img, w, h):
    img = img.convert('RGB').resize((w, h), Image.LANCZOS)
    px = img.load()
    out = bytearray(w * h * 2)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[i] = (v >> 8) & 0xFF
            out[i + 1] = v & 0xFF
            i += 2
    return bytes(out)
    
def clean_title(t):
    t = re.sub(r'\s*[\(\[【].*?(mv|m/v|official|audio|lyric|가사|live|ver).*?[\)\]】]',
               '', t, flags=re.I)
    t = re.sub(r'\s+', ' ', t).strip()
    return t


def split_artist_title(title, artist):
    title = clean_title(title)
    for sep in (' - ', ' – ', ' — '):
        if sep in title:
            a, t = title.split(sep, 1)
            return a.strip(), t.strip()
    return artist.strip(), title


def norm(x):
    return re.sub(r'[^0-9a-z가-힣]', '', x.lower())


def fetch_image(url, headers=None, timeout=5):
    req = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return Image.open(io.BytesIO(r.read()))


def art_from_plex(artist, title):
    if not PLEX_TOKEN:
        return None
    q = urllib.parse.quote(title)
    url = f'{PLEX_URL}/hubs/search?query={q}&limit=10&X-Plex-Token={PLEX_TOKEN}'
    req = urllib.request.Request(url, headers={'Accept': 'application/json'})
    with urllib.request.urlopen(req, timeout=5) as r:
        data = json.load(r)
    hubs = data.get('MediaContainer', {}).get('Hub', [])
    tracks = []
    for h in hubs:
        if h.get('type') == 'track':
            tracks += h.get('Metadata', [])
    if not tracks:
        return None

    na = norm(artist)
    nt = norm(title)
    best = None
    for m in tracks:
        ma = norm(m.get('grandparentTitle', '') + m.get('originalTitle', ''))
        mt = norm(m.get('title', ''))
        score = 0
        if nt and (nt in mt or mt in nt):
            score += 2
        if na and (na in ma or ma in na):
            score += 1
        if best is None or score > best[0]:
            best = (score, m)
    if best is None or best[0] == 0:
        return None
    m = best[1]
    thumb = m.get('thumb') or m.get('parentThumb') or m.get('grandparentThumb')
    if not thumb:
        return None
    return fetch_image(f'{PLEX_URL}{thumb}?X-Plex-Token={PLEX_TOKEN}')


def art_from_itunes(artist, title):
    term = urllib.parse.quote(f'{artist} {title}'.strip())
    url = (f'https://itunes.apple.com/search?term={term}'
           f'&country=kr&media=music&entity=song&limit=1')
    with urllib.request.urlopen(url, timeout=5) as r:
        data = json.load(r)
    if not data.get('resultCount'):
        return None
    art = data['results'][0].get('artworkUrl100')
    if not art:
        return None
    return fetch_image(art.replace('100x100', '600x600'))


def find_art(title, artist, session_thumb):
    a, t = split_artist_title(title, artist)
    for fn, name in ((art_from_plex, 'plex'), (art_from_itunes, 'itunes')):
        try:
            img = fn(a, t)
            if img is not None:
                print('art from', name)
                return img
        except Exception as e:
            print(name, 'art error:', e)
    if session_thumb is not None:
        print('art from session')
        return session_thumb
    return None

# =====================================================================
#  아이콘 캐시 / 전송
# =====================================================================
icon_cache = {1: [None] * 6, 2: [None] * 6, 3: [None] * 6}   # raw RGB565 bytes
cache_lock = threading.Lock()
board_page = 2                      # 보드가 보고 있는 페이지 (P:n 으로 갱신)


def set_icon(page, slot, img):
    raw = to_rgb565_bytes(img, ICON_W, ICON_H) if img is not None else None
    with cache_lock:
        icon_cache[page][slot - 1] = raw


def send_page(page):
    with cache_lock:
        raws = list(icon_cache[page])
    for i, raw in enumerate(raws):
        if raw:
            tx(('I:%d:%d:%d\n' % (page, i + 1, len(raw))).encode() + raw)


# =====================================================================
#  아이콘 추출 (IShellItemImageFactory)
# =====================================================================
class SIZE(ctypes.Structure):
    _fields_ = [('cx', c_int), ('cy', c_int)]


class IShellItemImageFactory(IUnknown):
    _iid_ = GUID('{BCC18B79-BA16-442F-80C4-8A59C30C463B}')
    _methods_ = [
        COMMETHOD([], HRESULT, 'GetImage',
                  (['in'], SIZE, 'size'),
                  (['in'], c_int, 'flags'),
                  (['out'], POINTER(wintypes.HBITMAP), 'phbm')),
    ]


_shell32 = ctypes.windll.shell32
_shell32.SHCreateItemFromParsingName.argtypes = [
    wintypes.LPCWSTR, ctypes.c_void_p, POINTER(GUID),
    POINTER(POINTER(IShellItemImageFactory))]
_shell32.SHCreateItemFromParsingName.restype = ctypes.HRESULT

SIIGBF_BIGGERSIZEOK = 0x1
SIIGBF_ICONONLY = 0x4


def shell_icon(path, size=64):
    import win32ui, win32gui
    factory = POINTER(IShellItemImageFactory)()
    hr = _shell32.SHCreateItemFromParsingName(
        path, None, byref(IShellItemImageFactory._iid_), byref(factory))
    if hr != 0:
        raise OSError(f'SHCreateItemFromParsingName 0x{hr & 0xFFFFFFFF:08X}')
    hbm = factory.GetImage(SIZE(size, size), SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK)
    bmp = win32ui.CreateBitmapFromHandle(hbm)
    info = bmp.GetInfo()
    bits = bmp.GetBitmapBits(True)
    img = Image.frombuffer('RGBA', (info['bmWidth'], info['bmHeight']),
                           bits, 'raw', 'BGRA', 0, 1)
    win32gui.DeleteObject(hbm)
    return img


def composite_premul(img, bg):
    img = img.convert('RGBA').resize((ICON_W, ICON_H), Image.LANCZOS)
    r, g, b, a = img.split()
    inv = ImageChops.invert(a)
    bg_img = Image.new('RGB', img.size, bg)
    bg_part = ImageChops.multiply(bg_img, Image.merge('RGB', (inv, inv, inv)))
    return ImageChops.add(Image.merge('RGB', (r, g, b)), bg_part)


def composite_straight(img, bg):
    img = img.convert('RGBA').resize((ICON_W, ICON_H), Image.LANCZOS)
    base = Image.new('RGBA', img.size, bg + (255,))
    return Image.alpha_composite(base, img).convert('RGB')


def icon_from_path(path):
    return composite_premul(shell_icon(path), BTN_BG)


# =====================================================================
#  바로가기 검색
# =====================================================================
def norm(x):
    return re.sub(r'[^0-9a-z가-힣]', '', x.lower())


def list_lnks(dirs):
    out = []
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                if f.lower().endswith('.lnk'):
                    out.append(os.path.join(root, f))
    return out


def find_lnk(keywords, lnks):
    kws = [norm(k) for k in keywords]
    ranked = []
    for p in lnks:
        stem = norm(os.path.splitext(os.path.basename(p))[0])
        for k in kws:
            if not k:
                continue
            if stem == k:
                ranked.append((0, p)); break
            if stem.startswith(k):
                ranked.append((1, p)); break
            if k in stem:
                ranked.append((2, p)); break
    ranked.sort()
    return ranked[0][1] if ranked else None


# =====================================================================
#  작업표시줄 순서 (UI Automation)
# =====================================================================
def taskbar_names():
    from pywinauto import Desktop
    tb = Desktop(backend='uia').window(class_name='Shell_TrayWnd')
    try:
        frame = tb.child_window(auto_id='TaskbarFrame').wrapper_object()
        items = frame.children()
    except Exception:
        items = tb.descendants(control_type='Button')
    names = []
    for c in items:
        ei = c.element_info
        if ei.control_type != 'Button':
            continue
        aid = ei.automation_id or ''
        if aid in SKIP_TASKBAR_IDS or aid.startswith('SystemTray') or aid.startswith('NotifyItem'):
            continue
        nm = (ei.name or '').strip()
        if not nm:
            continue
        nm = nm.split(' - ')[0]
        nm = re.sub(r'\s*(고정됨|pinned)\s*$', '', nm, flags=re.I).strip()
        names.append(nm)
    return names[:6]


_lnk_meta_cache = {}

def lnk_names(path):
    """바로가기 하나가 가질 수 있는 이름들: 파일명, 대상 exe 제품명/설명/파일명"""
    if path in _lnk_meta_cache:
        return _lnk_meta_cache[path]
    names = [os.path.splitext(os.path.basename(path))[0]]
    try:
        import win32com.client, win32api
        sh = win32com.client.Dispatch('WScript.Shell')
        target = sh.CreateShortcut(path).TargetPath
        if target and os.path.isfile(target):
            names.append(os.path.splitext(os.path.basename(target))[0])
            try:
                lang, cp = win32api.GetFileVersionInfo(target, r'\VarFileInfo\Translation')[0]
                for key in ('ProductName', 'FileDescription'):
                    v = win32api.GetFileVersionInfo(target, f'\\StringFileInfo\\{lang:04x}{cp:04x}\\{key}')
                    if v:
                        names.append(v)
            except Exception:
                pass
    except Exception:
        pass
    _lnk_meta_cache[path] = names
    return names


def match_pinned(name, pinned):
    n = norm(name)
    best, best_len = None, 1 << 30
    for p in pinned:
        for cand in lnk_names(p):
            c = norm(cand)
            if not c:
                continue
            if c == n:
                return p
            if (n in c or c in n) and len(c) < best_len:
                best, best_len = p, len(c)
    return best


# =====================================================================
#  페이지별 아이콘 준비
# =====================================================================
page3_lnk = [None] * 6
page2_names = []

def make_letter_icon(name, color=(0x4A, 0x90, 0xD9)):
    """아이콘을 못 구했을 때 첫 글자로 만드는 대체 아이콘"""
    from PIL import ImageDraw, ImageFont
    img = Image.new('RGB', (ICON_W, ICON_H), color)
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype(r'C:\Windows\Fonts\malgunbd.ttf', 28)
    except Exception:
        font = ImageFont.load_default()
    ch = name.strip()[:1].upper() or '?'
    bbox = d.textbbox((0, 0), ch, font=font)
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text(((ICON_W - w) / 2 - bbox[0], (ICON_H - h) / 2 - bbox[1]), ch, fill='white', font=font)
    return img


def load_icon_source(src):
    """로컬 파일 경로 또는 URL → PIL Image"""
    if os.path.isfile(src):
        return Image.open(src)
    req = urllib.request.Request(src, headers={'User-Agent': 'Mozilla/5.0'})
    with urllib.request.urlopen(req, timeout=5) as r:
        return Image.open(io.BytesIO(r.read()))

def build_page1():
    for i, fav in enumerate(FAVORITES[:6]):
        name, url = fav[0], fav[1]
        explicit = fav[2] if len(fav) > 2 else None
        u = urllib.parse.urlparse(url)
        candidates = []
        if explicit:
            candidates.append(explicit)
        candidates.append(f'{u.scheme}://{u.netloc}/favicon.ico')           # 사이트 자체 파비콘
        candidates.append(f'https://www.google.com/s2/favicons?domain={u.netloc}&sz=64')
        img = None
        for src in candidates:
            try:
                img = load_icon_source(src)
                break
            except Exception:
                continue
        if img is None:
            img = make_letter_icon(name)
            print(f'p1 icon {i+1}: {name} (letter fallback)')
        else:
            print(f'p1 icon {i+1}: {name}')
        set_icon(1, i + 1, composite_straight(img, BTN_BG))

def build_page2():
    """작업표시줄 순서가 바뀌었으면 True"""
    global page2_names
    names = taskbar_names()
    if names == page2_names:
        return False
    page2_names = names
    pinned = list_lnks([TASKBAR_PIN_DIR])
    start = None
    for i in range(6):
        if i >= len(names):
            set_icon(2, i + 1, Image.new('RGB', (ICON_W, ICON_H), BTN_BG))
            continue
        nm = names[i]
        p = match_pinned(nm, pinned)
        if p is None:
            if start is None:
                start = list_lnks(START_DIRS)
            kws = NAME_ALIASES.get(nm.lower(), []) + [nm]
            for k in kws:
                if k.lower().endswith('.exe') or k.lower().startswith('shell:'):
                    p = k            # 경로 직접 지정
                    break
            if p is None:
                p = find_lnk(kws, start)
        try:
            if p:
                set_icon(2, i + 1, icon_from_path(p))
                print(f'p2 icon {i+1}: {nm} -> {os.path.basename(p)}')
            else:
                set_icon(2, i + 1, Image.new('RGB', (ICON_W, ICON_H), BTN_BG))
                print(f'p2 icon {i+1}: {nm} (no shortcut)')
        except Exception as e:
            print(f'p2 icon {i+1} error ({nm}):', e)
    return True


def build_page3():
    lnks = list_lnks(START_DIRS)
    for i, (name, kws) in enumerate(PAGE3_APPS[:6]):
        p = find_lnk(kws, lnks)
        page3_lnk[i] = p
        try:
            if p:
                set_icon(3, i + 1, icon_from_path(p))
                print(f'p3 icon {i+1}: {name} -> {os.path.basename(p)}')
            else:
                print(f'p3 icon {i+1}: {name} (not found)')
        except Exception as e:
            print(f'p3 icon {i+1} error ({name}):', e)


def icon_worker():
    comtypes.CoInitialize()
    time.sleep(2)
    try:
        build_page1()
        if board_page == 1: send_page(1)
    except Exception as e:
        print('page1 error:', e)
    try:
        build_page3()
        if board_page == 3: send_page(3)
    except Exception as e:
        print('page3 error:', e)
    while True:
        try:
            if build_page2() and board_page == 2:
                send_page(2)
        except Exception as e:
            print('page2 error:', e)
        time.sleep(10)


# =====================================================================
#  버튼 동작
# =====================================================================
def on_button(page, slot):
    if page == 1:
        if slot <= len(FAVORITES):
            webbrowser.open(FAVORITES[slot - 1][1])
    elif page == 2:
        pyautogui.hotkey('win', str(slot))
    elif page == 3:
        p = page3_lnk[slot - 1] if slot <= 6 else None
        if p:
            os.startfile(p)
        else:
            print('page3 slot', slot, 'has no shortcut')


# =====================================================================
#  앨범 커버 색
# =====================================================================
def cover_colors(img):
    small = img.convert('RGB').resize((50, 50))
    pal = small.quantize(colors=8, method=Image.Quantize.MEDIANCUT).convert('RGB')
    counts = sorted(pal.getcolors(2500), reverse=True)
    vivid = []
    for _, (r, g, b) in counts:
        if max(r, g, b) - min(r, g, b) > 40:
            vivid.append((r, g, b))
    if not vivid:
        vivid = [counts[0][1]]
    bg = vivid[0]
    fg = vivid[1] if len(vivid) > 1 else vivid[0]
    bg = tuple(int(c * 0.45) for c in bg)
    fg = tuple(min(255, int(c * 0.5 + 128)) for c in fg)
    return bg, fg


# =====================================================================
#  곡 정보 정리
# =====================================================================
def clean_title(t):
    t = re.sub(r'\s*[\(\[【].*?(mv|m/v|official|audio|lyric|가사|live|ver).*?[\)\]】]', '', t, flags=re.I)
    t = re.sub(r'\s+', ' ', t).strip()
    return t


def split_artist_title(title, artist):
    title = clean_title(title)
    for sep in (' - ', ' – ', ' — '):
        if sep in title:
            a, t = title.split(sep, 1)
            return a.strip(), t.strip()
    return artist.strip(), title


def http_json(url, headers=None, timeout=5):
    req = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def http_text(url, headers=None, timeout=5):
    req = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode('utf-8', errors='ignore')


def fetch_image(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return Image.open(io.BytesIO(r.read()))


# =====================================================================
#  Plex
# =====================================================================
def plex_ok():
    return PLEX_TOKEN and not PLEX_TOKEN.startswith('여기에')


def plex_find_track(artist, title):
    if not plex_ok() or not title.strip():
        return None
    q = urllib.parse.quote(title)
    url = f'{PLEX_URL}/hubs/search?query={q}&limit=20&X-Plex-Token={PLEX_TOKEN}'
    data = http_json(url, headers={'Accept': 'application/json'})
    tracks = []
    for h in data.get('MediaContainer', {}).get('Hub', []):
        if h.get('type') == 'track':
            tracks += h.get('Metadata', [])
    if not tracks:
        return None
    na, nt = norm(artist), norm(title)
    best = None
    for m in tracks:
        ma = norm(m.get('grandparentTitle', '') + m.get('originalTitle', ''))
        mt = norm(m.get('title', ''))
        album = m.get('parentTitle', '') + m.get('grandparentTitle', '')
        score = 0
        if nt and (nt in mt or mt in nt):
            score += 2
        if na and (na in ma or ma in na):
            score += 3                       # 진짜 아티스트 앨범이면 크게 가산
        if COMP_RE.search(album):
            score -= 2                 # 차트/모음집 폴더는 감점
        if best is None or score > best[0]:
            best = (score, m)
    if best is None or best[0] <= 0:
        return None
    m = best[1]
    m['_artist_match'] = best[0] >= 5        # 제목+가수 둘 다 맞음
    return m


def plex_art(track):
    for key in ('thumb', 'parentThumb'):
        thumb = track.get(key)
        if not thumb:
            continue
        try:
            return fetch_image(f'{PLEX_URL}{thumb}?X-Plex-Token={PLEX_TOKEN}')
        except Exception as e:
            print(f'plex {key} error:', e)
    return None


def plex_lrc(track):
    key = track.get('ratingKey')
    if not key:
        return None
    url = f'{PLEX_URL}/library/metadata/{key}?X-Plex-Token={PLEX_TOKEN}'
    data = http_json(url, headers={'Accept': 'application/json'})
    for m in data.get('MediaContainer', {}).get('Metadata', []):
        for media in m.get('Media', []):
            for part in media.get('Part', []):
                for st in part.get('Stream', []):
                    if st.get('streamType') == 4:
                        sid = st.get('id')
                        txt = http_text(f'{PLEX_URL}/library/streams/{sid}?X-Plex-Token={PLEX_TOKEN}')
                        if '[' in txt:
                            return txt
    return None


def plex_now_playing():
    """Plex 서버 세션(Plexamp 등) 중 음악 트랙
       → (title, artist, pos, end, playing, track_dict) / None"""
    if not plex_ok():
        return None
    url = f'{PLEX_URL}/status/sessions?X-Plex-Token={PLEX_TOKEN}'
    root = ET.fromstring(http_text(url, timeout=3))
    best = None
    for tr in root.findall('Track'):
        player = tr.find('Player')
        state = player.get('state', '') if player is not None else ''
        playing = 1 if state == 'playing' else 0
        pos = int(tr.get('viewOffset', '0')) / 1000.0
        end = int(int(tr.get('duration', '0')) / 1000)
        track = {
            'ratingKey': tr.get('ratingKey'),
            'thumb': tr.get('thumb'),
            'parentThumb': tr.get('parentThumb'),
            'grandparentThumb': tr.get('grandparentThumb'),
            'title': tr.get('title', ''),
            'grandparentTitle': tr.get('grandparentTitle', ''),
            'parentTitle': tr.get('parentTitle', ''),
            'parentRatingKey': tr.get('parentRatingKey'),
            'index': int(tr.get('index', '0') or 0),
        }
        item = (tr.get('title', ''), tr.get('grandparentTitle', ''), pos, end, playing, track)
        if playing:
            return item
        if best is None:
            best = item
    return best


# =====================================================================
#  iTunes / LRCLIB
# =====================================================================
def name_variants(s):
    """'아이오아이 (I.O.I)' → ['아이오아이 (I.O.I)', '아이오아이', 'I.O.I']"""
    return title_variants(s)


def itunes_art(artist, title):
    nt, na = norm(title), norm(artist)
    tried = set()
    for a in name_variants(artist):
        for t in title_variants(title):
          for term_s, country in ((f'{a} {t}', 'us'), (t, 'us'), (f'{a} {t}', 'kr')):
            if (term_s, country) in tried:
                continue
            tried.add((term_s, country))
            term = urllib.parse.quote(term_s)
            url = f'https://itunes.apple.com/search?term={term}&country={country}&media=music&entity=song&limit=10'
            try:
                res = http_json(url).get('results', [])
            except Exception as e:
                print('itunes http error:', e)
                continue
            print(f'itunes q=({term_s} @{country}) ->', [(r.get('artistName'), r.get('trackName')) for r in res[:3]])
            for i, r in enumerate(res):
                rt, ra = norm(r.get('trackName', '')), norm(r.get('artistName', ''))
                tv, av = norm(t), norm(a)
                title_ok = (tv in rt or rt in tv or nt in rt)
                artist_ok = (av in ra or ra in av or na in ra or not av)
                ok = (title_ok and artist_ok)                       # 둘 다 일치
                ok = ok or (title_ok and tv == rt)                  # 제목 정확 일치 (가수 표기 다름: BTS/방탄소년단)
                ok = ok or (i == 0 and artist_ok and term_s != t)   # 가수+제목으로 검색해 1순위가 같은 가수 (영문 제목 등록곡)
                if ok:
                    art = r.get('artworkUrl100', '')
                    if art:
                        return fetch_image(art.replace('100x100', '300x300'))
    return None


def deezer_art(artist, title):
    nt = norm(title)
    for a in name_variants(artist):
        for t in title_variants(title):
            q = urllib.parse.quote(f'artist:"{a}" track:"{t}"')
            try:
                res = http_json(f'https://api.deezer.com/search?q={q}&limit=5').get('data', [])
            except Exception:
                continue
            for r in res:
                rt = norm(r.get('title', ''))
                if norm(t) in rt or rt in norm(t) or nt in rt:
                    cover = (r.get('album') or {}).get('cover_big') or (r.get('album') or {}).get('cover_medium')
                    if cover:
                        return fetch_image(cover)
    return None


def title_variants(title):
    """'뛰어(JUMP)' → ['뛰어(JUMP)', '뛰어', 'JUMP']"""
    v = [title]
    m = re.match(r'^(.*?)\s*[\(（]([^)）]*)[\)）]\s*$', title)
    if m:
        a, b = m.group(1).strip(), m.group(2).strip()
        if a: v.append(a)
        if b and not re.search(r'prod|feat|ver|remix|inst', b, re.I): v.append(b)
    return list(dict.fromkeys(x for x in v if x))


def lrclib_lrc(artist, title, duration):
    hdr = {'User-Agent': 'deck-lcd/1.0'}
    nt = norm(title)
    want_ko = has_hangul(artist) or has_hangul(title)
    for t in title_variants(title):
        # 1) 정확 조회
        try:
            q = urllib.parse.urlencode({'artist_name': artist, 'track_name': t, 'duration': duration})
            d = http_json(f'https://lrclib.net/api/get?{q}', headers=hdr)
            if d.get('syncedLyrics') and (not want_ko or has_hangul(d['syncedLyrics'])):
                return d['syncedLyrics']
        except Exception:
            pass
        # 2) 가수+제목 검색 → 3) 제목만 검색
        for params in ({'track_name': t, 'artist_name': artist}, {'track_name': t}):
            try:
                q = urllib.parse.urlencode(params)
                res = http_json(f'https://lrclib.net/api/search?{q}', headers=hdr)
            except Exception:
                continue
            ntv = norm(t)
            for d in res:
                if d.get('syncedLyrics') and (ntv in norm(d.get('trackName', '')) or nt in norm(d.get('trackName', ''))):
                    if abs(int(d.get('duration', 0) or 0) - duration) <= 5 or not duration:
                        if not want_ko or has_hangul(d['syncedLyrics']):
                            return d['syncedLyrics']
            for d in res:
                if d.get('syncedLyrics') and abs(int(d.get('duration', 0) or 0) - duration) <= 3:
                    if not want_ko or has_hangul(d['syncedLyrics']):
                        return d['syncedLyrics']
    return None


def netease_lrc(artist, title, duration):
    """NetEase 클라우드뮤직 싱크 가사 (K-pop 커버리지 좋음)"""
    hdr = {'User-Agent': 'Mozilla/5.0', 'Referer': 'https://music.163.com/'}
    for t in title_variants(title):
        try:
            q = urllib.parse.urlencode({'s': f'{artist} {t}', 'type': 1, 'limit': 8})
            d = http_json(f'https://music.163.com/api/search/get?{q}', headers=hdr)
            songs = d.get('result', {}).get('songs', []) or []
        except Exception:
            continue
        ntv = norm(t)
        best = None
        for sng in songs:
            name = norm(sng.get('name', ''))
            dur = int(sng.get('duration', 0)) / 1000
            if ntv in name or name in ntv:
                if not duration or abs(dur - duration) <= 5:
                    best = sng; break
                if best is None: best = sng
        if not best:
            continue
        try:
            d = http_json(f"https://music.163.com/api/song/lyric?id={best['id']}&lv=1&kv=0&tv=0", headers=hdr)
            lrc = (d.get('lrc') or {}).get('lyric', '')
            if lrc and '[' in lrc and LRC_RE.search(lrc):
                return lrc
        except Exception:
            continue
    return None


LRC_RE = re.compile(r'\[(\d+):(\d+(?:\.\d+)?)\]')
HANGUL_RE = re.compile(r'[가-힣]')

def has_hangul(s):
    return bool(HANGUL_RE.search(s or ''))


def parse_lrc(text):
    out = []
    for line in text.splitlines():
        stamps = LRC_RE.findall(line)
        if not stamps:
            continue
        lyric = LRC_RE.sub('', line).strip()
        for mm, ss in stamps:
            out.append((int(mm) * 60 + float(ss), lyric))
    out.sort()
    return out


def lyric_at(lines, t):
    """(이전, 현재, 다음) 가사 줄"""
    idx = -1
    for i, (ts, txt) in enumerate(lines):
        if ts <= t:
            idx = i
        else:
            break
    if idx < 0:
        return ('', '', lines[0][1] if lines else '')
    prev = lines[idx - 1][1] if idx > 0 else ''
    cur = lines[idx][1]
    nxt = lines[idx + 1][1] if idx + 1 < len(lines) else ''
    return (prev, cur, nxt)



def lookup_song(artist, title, duration, session_thumb, track=None):
    a, t = split_artist_title(title, artist)
    art = None
    lrc = None

    # ---- 캐시 ----
    key = cache_key(a, t)
    art_path = os.path.join(CACHE_DIR, key + '.png')
    lrc_path = os.path.join(CACHE_DIR, key + '.json')
    if os.path.isfile(art_path) and os.path.isfile(lrc_path):
        try:
            art = Image.open(art_path)
            art.load()
            with open(lrc_path, encoding='utf-8') as f:
                lines = [tuple(x) for x in json.load(f)]
            print(f'cache hit: {a} / {t}')
            return art, lines
        except Exception as e:
            print('cache read error:', e)
            art = None
    is_comp = bool(track) and bool(COMP_RE.search(
        track.get('parentTitle', '') + track.get('grandparentTitle', '')))
    if track is None or is_comp:
        try:
            found = plex_find_track(a, t)
            if found:
                track = found
                print('plex: matched library track', found.get('parentTitle', ''))
        except Exception as e:
            print('plex error:', e)
    def try_plex():
        if not track:
            return None
        if COMP_RE.search(track.get('parentTitle', '') + track.get('grandparentTitle', '')):
            return None
        try:
            img = plex_art(track)
            if img: print(f'art: plex ({a} / {t})')
            return img
        except Exception as e:
            print('plex art error:', e)
            return None
    def try_itunes():
        try:
            img = itunes_art(a, t)
            if img: print(f'art: itunes ({a} / {t})')
            return img
        except Exception as e:
            print('itunes error:', e)
            return None        
    def try_deezer():
        try:
            img = deezer_art(a, t)
            if img: print(f'art: deezer ({a} / {t})')
            return img
        except Exception as e:
            print('deezer error:', e)
            return None

    plex_good = bool(track) and track.get('_artist_match', False)
    if plex_good:
        order = (try_plex, try_itunes, try_deezer)
    else:
        order = (try_itunes, try_deezer, try_plex)
    for fn in order:
        art = fn()
        if art is not None:
            break
    if art is None and track and track.get('grandparentThumb'):
        try:
            art = fetch_image(f"{PLEX_URL}{track['grandparentThumb']}?X-Plex-Token={PLEX_TOKEN}")
            print('art: plex artist image (fallback)')
        except Exception as e:
            print('plex artist image error:', e)
    if art is None and session_thumb is not None:
        art = session_thumb
        print('art: session thumb')

    # ---- 가사: LRCLIB → NetEase → Plex .lrc ----
    want_ko = has_hangul(a) or has_hangul(t)
    fallback = None
    sources = [('lrclib', lambda: lrclib_lrc(a, t, duration)),
               ('netease', lambda: netease_lrc(a, t, duration))]
    if track:
        sources.append(('plex', lambda: plex_lrc(track)))
    for name, fn in sources:
        try:
            cand = fn()
        except Exception as e:
            print(f'{name} error:', e)
            continue
        if not cand:
            continue
        if want_ko and not has_hangul(cand):
            if fallback is None:
                fallback = (name, cand)
            continue
        lrc = cand
        print(f'lyrics: {name}')
        break
    if lrc is None and fallback:
        lrc = fallback[1]
        print(f'lyrics: {fallback[0]} (romanized)')
    if lrc is None:
        print('lyrics: none')
    lines = parse_lrc(lrc) if lrc else []

    # ---- 캐시 저장 (아트 있을 때만 — 없으면 다음에 다시 시도) ----
    if art is not None:
        try:
            art.convert('RGB').resize((ART_W, ART_H), Image.LANCZOS).save(art_path)
            with open(lrc_path, 'w', encoding='utf-8') as f:
                json.dump(lines, f, ensure_ascii=False)
        except Exception as e:
            print('cache write error:', e)
    return art, lines
   
prefetch_lock = threading.Lock()


def plex_next_track(track):
    """같은 앨범에서 다음 index 트랙 → (title, artist, duration, track_dict) / None"""
    pk, idx = track.get('parentRatingKey'), track.get('index')
    if not pk or not idx:
        return None
    url = f'{PLEX_URL}/library/metadata/{pk}/children?X-Plex-Token={PLEX_TOKEN}'
    items = http_json(url, headers={'Accept': 'application/json'}).get('MediaContainer', {}).get('Metadata', [])
    items.sort(key=lambda m: int(m.get('index', 0) or 0))
    for m in items:
        if int(m.get('index', 0) or 0) > idx:
            nt = {
                'ratingKey': m.get('ratingKey'),
                'thumb': m.get('thumb'),
                'parentThumb': m.get('parentThumb'),
                'grandparentThumb': m.get('grandparentThumb'),
                'title': m.get('title', ''),
                'grandparentTitle': m.get('grandparentTitle', ''),
                'parentTitle': m.get('parentTitle', ''),
                'parentRatingKey': m.get('parentRatingKey'),
                'index': int(m.get('index', 0) or 0),
            }
            dur = int(int(m.get('duration', 0) or 0) / 1000)
            return m.get('title', ''), m.get('grandparentTitle', ''), dur, nt
    return None


def prefetch_next(track):
    """다음 곡 아트·가사를 미리 찾아 캐시에 넣음 (백그라운드)"""
    if not prefetch_lock.acquire(blocking=False):
        return
    try:
        r = plex_next_track(track)
        if not r:
            return
        title, artist, dur, nt = r
        a, t = split_artist_title(title, artist)
        if os.path.isfile(os.path.join(CACHE_DIR, cache_key(a, t) + '.png')):
            return
        print(f'prefetch: {a} / {t}')
        lookup_song(artist, title, dur, None, nt)
    except Exception as e:
        print('prefetch error:', e)
    finally:
        prefetch_lock.release()

# =====================================================================
#  Windows 미디어 세션
# =====================================================================
def pick_session(mgr):
    """재생 중인 세션 우선, 없으면 현재 세션"""
    try:
        for sess in mgr.get_sessions():
            if sess.get_playback_info().playback_status == 4:   # Playing
                return sess
    except Exception:
        pass
    return mgr.get_current_session()


async def get_thumb(sess):
    info = await sess.try_get_media_properties_async()
    if info.thumbnail is None:
        return None
    stream = await info.thumbnail.open_read_async()
    size = stream.size
    if size == 0:
        return None
    from winsdk.windows.storage.streams import Buffer, InputStreamOptions
    buf = Buffer(size)
    await stream.read_async(buf, size, InputStreamOptions.READ_AHEAD)
    from winsdk.windows.security.cryptography import CryptographicBuffer
    data = bytes(CryptographicBuffer.copy_to_byte_array(buf))
    return Image.open(io.BytesIO(data))


async def get_media():
    mgr = await MediaManager.request_async()
    sess = pick_session(mgr)
    if sess is None:
        return None
    info = await sess.try_get_media_properties_async()
    tl = sess.get_timeline_properties()
    pi = sess.get_playback_info()
    pos = tl.position.total_seconds() if tl.position else 0.0
    end = int(tl.end_time.total_seconds()) if tl.end_time else 0
    playing = 1 if pi.playback_status == 4 else 0
    return (info.title or '', info.artist or '', pos, end, playing)


async def get_media_thumb():
    mgr = await MediaManager.request_async()
    sess = pick_session(mgr)
    if sess is None:
        return None
    try:
        return await get_thumb(sess)
    except Exception as e:
        print('thumb error:', e)
        return None


# =====================================================================
#  센서
# =====================================================================
def collect(node, out):
    sid = node.get('SensorId')
    if sid:
        out[sid] = node.get('Value', '')
    for c in node.get('Children', []):
        collect(c, out)


def num(text):
    try:
        return float(text.split()[0])
    except (ValueError, IndexError):
        return 0.0
        
# 허용할 알림: (앱 이름에 포함될 키워드, 제목/본문에 포함되어야 하는 키워드 목록 또는 None=전부)
TOAST_ALLOW = [
    ('카카오톡', None),
    ('kakaotalk', None),
    ('휴대폰과 연결', None),        # 문자(Phone Link)
    ('phone link', None),
    ('메일플러그', None),
    ('mailplug', None),
    ('outlook', None),
    ('메일', None),                 # Windows 메일 앱
    ('mail', None),
    # 브라우저 웹 알림은 메일 관련일 때만
    ('chrome', ['gmail', 'mail', '메일', '@']),
    ('웨일', ['gmail', 'mail', '메일', '@']),
    ('whale', ['gmail', 'mail', '메일', '@']),
]


def toast_allowed(app, title, body):
    a = app.lower()
    text = (title + ' ' + body).lower()
    for key, words in TOAST_ALLOW:
        if key in a:
            if words is None or any(w in text for w in words):
                return True
    return False


def toast_fields(n):
    """UserNotification → (앱이름, 제목, 본문)"""
    app = ''
    try:
        app = n.app_info.display_info.display_name or ''
    except Exception:
        pass
    title, body = '', ''
    try:
        binding = n.notification.visual.get_binding('ToastGeneric')
        if binding:
            texts = [t.text for t in binding.get_text_elements()]
            texts = [t for t in texts if t]
            if texts:
                title = texts[0]
                body = ' '.join(texts[1:])
    except Exception:
        pass
    return app, title, body


async def toast_poll(listener, seen):
    ns = await listener.get_notifications_async(NotificationKinds.TOAST)
    new = []
    for n in ns:
        if n.id in seen:
            continue
        seen.add(n.id)
        new.append(n)
    return new

async def toast_request_access(listener):
    return await listener.request_access_async()

def toast_worker():
    try:
        listener = UserNotificationListener.current
        status = asyncio.run(toast_request_access(listener))
        if status != UserNotificationListenerAccessStatus.ALLOWED:
            print('toast: access not allowed (설정 > 개인정보 > 알림 에서 허용 필요)', status)
            return
    except Exception as e:
        print('toast init error:', e)
        return
    seen = set()
    first = True
    while True:
        try:
            new = asyncio.run(toast_poll(listener, seen))
            if first:                       # 시작 시 이미 쌓여 있던 알림은 무시
                first = False
                new = []
            for n in new:
                app, title, body = toast_fields(n)
                if not title and not body:
                    continue
                if not toast_allowed(app, title, body):
                    print('toast skip:', app, '/', title[:30])
                    continue
                clean = lambda x: x.replace('|', ' ').replace('\n', ' ').replace('\r', ' ')
                msg = 'T:%s|%s|%s\n' % (clean(app)[:30], clean(title)[:60], clean(body)[:100])
                tx(msg.encode('utf-8'))
                print('toast:', app, '/', title, '/', body[:40])
        except Exception as e:
            print('toast error:', e)
        time.sleep(1)

now_playing = False
screen_on = True
last_play = time.time()


def win_idle_seconds():
    class LASTINPUTINFO(ctypes.Structure):
        _fields_ = [('cbSize', ctypes.c_uint), ('dwTime', ctypes.c_uint)]
    li = LASTINPUTINFO()
    li.cbSize = ctypes.sizeof(li)
    ctypes.windll.user32.GetLastInputInfo(ctypes.byref(li))
    return (ctypes.windll.kernel32.GetTickCount() - li.dwTime) / 1000.0


def set_screen(on):
    global screen_on
    if on != screen_on:
        screen_on = on
        tx(b'D:1\n' if on else b'D:0\n')
        print('screen', 'on' if on else 'off')


def idle_worker():
    global last_play
    while True:
        try:
            h = time.localtime().tm_hour
            night = (h >= NIGHT_OFF_HOUR or h < NIGHT_ON_HOUR)

            if night and not now_playing:
                if time.time() - last_touch < NIGHT_WAKE_SEC:
                    set_screen(True)
                else:
                    set_screen(False)
            elif now_playing:
                last_play = time.time()
                set_screen(True)
            elif time.time() - last_play > IDLE_OFF_SEC:
                set_screen(False)
            else:
                set_screen(True)
        except Exception as e:
            print('idle error:', e)
        time.sleep(10)
        
def sensor_worker():
    while True:
        try:
            data = json.load(urllib.request.urlopen(LHM_URL, timeout=2))
            vals = {}
            collect(data, vals)
            v = [num(vals.get(SENSORS[k], '')) for k in
                 ('cpu_load', 'cpu_temp', 'gpu_load', 'gpu_temp', 'ram_load')]
            used = num(vals.get(SENSORS['ram_used'], ''))
            total = used + num(vals.get(SENSORS['ram_avail'], ''))
            line = 'S:%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n' % (
                v[0], v[1], v[2], v[3], v[4], used * 10, total * 10)
            tx(line.encode())
        except Exception as e:
            print('sensor error:', e)
        time.sleep(1)


# =====================================================================
#  음악  (SMTC 재생 중 > Plex 세션 재생 중 > 마지막 세션 일시정지)
# =====================================================================
def media_worker():
    global reconnected, now_playing
    last_key = None
    base_pos = 0.0
    base_time = 0.0
    lyrics = []
    last_lyric = None
    src_last = None
    plex_last_pos = None
    plex_same = 0
    while True:
        if reconnected:
            reconnected = False
            last_key = None
        try:
            m = None
            try:
                m = asyncio.run(get_media())
            except Exception as e:
                print('smtc error:', e)
            track = None
            src = 'smtc'

            if m is None or not m[4]:
                try:
                    pm = plex_now_playing()
                except Exception as e:
                    pm = None
                    print('plex sessions error:', e)
                if pm and (m is None or pm[4]):
                    title, artist, pos, end, playing, track = pm
                    if playing and pos == plex_last_pos:
                        plex_same += 1
                        if plex_same >= 50:          # 25초 동안 viewOffset 그대로 → 유령 세션
                            playing = 0
                    else:
                        plex_same = 0
                    plex_last_pos = pos
                    m = (title, artist, pos, end, playing)
                    src = 'plex'

            cur_src = src if m else None
            if cur_src != src_last:
                print('source:', cur_src or 'none')
                src_last = cur_src

            if m:
                title, artist, pos, end, playing = m
                global now_playing
                now_playing = bool(playing)
                key = (title, artist, end)

                new_song = (key != last_key)
                if new_song or abs(pos - base_pos) > 2:
                    base_pos = pos
                    base_time = time.time()

                if playing:
                    cur = base_pos + (time.time() - base_time)
                else:
                    cur = base_pos
                if end > 0 and cur > end:
                    cur = float(end)

                a_disp, t_disp = split_artist_title(title, artist)
                t_disp = t_disp.replace('|', ' ').replace('\n', ' ')
                a_disp = a_disp.replace('|', ' ').replace('\n', ' ')
                line = 'M:%s|%s|%d|%d|%d\n' % (t_disp, a_disp, int(cur), end, playing)
                tx(line.encode('utf-8'))

                if new_song:
                    last_key = key
                    last_lyric = None
                    art_sent = False
                    art_try = 0
                    tx(b'L:||\n')
                    thumb = None
                    if src == 'smtc':
                        thumb = asyncio.run(get_media_thumb())
                    art, lyrics = lookup_song(artist, title, end, thumb, track)
                    if art is not None:
                        raw = to_rgb565_bytes(art, ART_W, ART_H)
                        tx(('A:%d\n' % len(raw)).encode() + raw)
                        (r, g, b), (fr, fg, fb) = cover_colors(art)
                        tx(('G:%d,%d,%d,%d,%d,%d\n' % (r, g, b, fr, fg, fb)).encode())
                        art_sent = True
                        print('art sent', len(raw))
                    else:
                        print('no art (try 0)')
                    if src == 'plex' and track:
                        threading.Thread(target=prefetch_next, args=(track,), daemon=True).start()

                elif not art_sent and art_try < 5:
                    art_try += 1
                    thumb = None
                    if src == 'smtc':
                        thumb = asyncio.run(get_media_thumb())
                    art, ly2 = lookup_song(artist, title, end, thumb, track)
                    if not lyrics and ly2:
                        lyrics = ly2
                    if art is not None:
                        raw = to_rgb565_bytes(art, ART_W, ART_H)
                        tx(('A:%d\n' % len(raw)).encode() + raw)
                        (r, g, b), (fr, fg, fb) = cover_colors(art)
                        tx(('G:%d,%d,%d,%d,%d,%d\n' % (r, g, b, fr, fg, fb)).encode())
                        art_sent = True
                        print('art sent (retry %d)' % art_try)
                    else:
                        print('no art (try %d)' % art_try)

                if lyrics:
                    ly = lyric_at(lyrics, cur + LYRIC_OFFSET)
                    if ly != last_lyric:
                        last_lyric = ly
                        clean = lambda x: x.replace('|', ' ').replace('\n', ' ')
                        tx(('L:%s|%s|%s\n' % (clean(ly[0]), clean(ly[1]), clean(ly[2]))).encode('utf-8'))
        except Exception as e:
            print('media error:', e)
        time.sleep(0.5)


# =====================================================================
#  메인
# =====================================================================
threading.Thread(target=sensor_worker, daemon=True).start()
threading.Thread(target=media_worker, daemon=True).start()
threading.Thread(target=icon_worker, daemon=True).start()
threading.Thread(target=toast_worker, daemon=True).start()
threading.Thread(target=idle_worker, daemon=True).start()

while True:
    if s is None:
        open_port()
        continue
    try:
        line = s.readline().decode(errors='ignore').strip()
    except Exception:          # SerialException 뿐 아니라 닫힌 포트 오류도
        print('port lost')
        close_port()
        continue
    if not line:
        continue

    if line.startswith('acc'):
        print(line)
        continue    
    
    if line.startswith('O:'):
        print('orient', line[2:])
        continue
        
    if line.startswith('X:'):
        print(line)
        continue        
        
    if line.startswith('W:'):
        last_touch = time.time()
        last_play = time.time()
        set_screen(True)
        continue
        
    if line.startswith('P:'):                 # 보드 페이지 변경 → 그 페이지 아이콘 전송
        try:
            board_page = int(line[2:])
            send_page(board_page)
            print('page', board_page)
        except ValueError:
            pass
        continue

    if line.startswith('C:'):
        cmd = line[2:]
        if cmd == 'PLAY':
            pyautogui.press('playpause')
        elif cmd == 'NEXT':
            pyautogui.press('nexttrack')
        elif cmd == 'PREV':
            pyautogui.press('prevtrack')
        print('cmd', cmd)
        continue

    if not line.startswith('B:'):
        continue
    try:
        _, p, b = line.split(':')
        page, slot = int(p), int(b)
    except ValueError:
        continue
    print('pressed', page, slot)
    try:
        on_button(page, slot)
    except Exception as e:
        print('button error:', e)
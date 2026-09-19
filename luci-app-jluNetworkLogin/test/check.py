#!/usr/bin/env python3
"""
一致性检查（本地与 CI 都能跑，只用标准库）：

  1. 出厂默认值一致：jluNetworkLogin/files/jlu-network-login.config 里
     interface 的默认值 == LuCI 视图里同名选项的 o.default
  2. 视图里每个 _('...') 文案都要在 po 里有译文，po 里也不能有失效条目（pot 同步）
  3. 所有 JSON 文件可解析；视图能通过 node --check（有 node 时）

用法： python3 luci-app-jluNetworkLogin/test/check.py
"""
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.dirname(HERE)                  # luci-app-jluNetworkLogin
REPO = os.path.dirname(PKG)                  # 仓库根

VIEW = os.path.join(PKG, 'root/www/luci-static/resources/view/jluNetworkLogin.js')
PO = os.path.join(PKG, 'po/zh_Hans/luci-app-jluNetworkLogin.po')
POT = os.path.join(PKG, 'po/templates/luci-app-jluNetworkLogin.pot')
MENU = os.path.join(PKG, 'root/usr/share/luci/menu.d/luci-app-jluNetworkLogin.json')
CONF = os.path.join(REPO, 'jluNetworkLogin/files/jlu-network-login.config')

fails = 0


def check(ok, msg):
    global fails
    print(('  ok   ' if ok else '  FAIL ') + msg)
    if not ok:
        fails += 1


def js_option_default(js, option):
    m = re.search(r"option\([^)]*'%s'[^)]*\).*?o\.default\s*=\s*'([^']*)';" % option, js, re.S)
    return m.group(1) if m else None


print('== 1. 出厂默认值一致 ==')
js = open(VIEW, encoding='utf-8').read()
conf = open(CONF, encoding='utf-8').read()

js_if = js_option_default(js, 'interface')
conf_if = (re.search(r"option interface\s+'([^']*)'", conf) or [None, None])[1]
check(js_if is not None and js_if == conf_if,
      "interface 默认值: 视图 '%s' == 出厂配置 '%s'" % (js_if, conf_if))

print('== 2. 翻译覆盖 ==')
po = open(PO, encoding='utf-8').read()
po_ids = {}
for i, t in re.findall(r'^msgid "(.*)"\nmsgstr "(.*)"$', po, re.M):
    if i:
        po_ids[json.loads('"%s"' % i)] = json.loads('"%s"' % t)

title = list(json.load(open(MENU, encoding='utf-8')).values())[0]['title']
code_ids = set(re.findall(r"_\(\s*'((?:[^'\\]|\\.)*)'", js)) | {title}
check(not (code_ids - set(po_ids)), '视图里所有文案都有 po 条目（缺：%s）' % sorted(code_ids - set(po_ids)))
check(not (set(po_ids) - code_ids), 'po 里没有失效条目（多：%s）' % sorted(set(po_ids) - code_ids))
check(all(v.strip() for v in po_ids.values()), '所有译文都非空')

pot_ids = {json.loads('"%s"' % i)
           for i in re.findall(r'^msgid "(.*)"$', open(POT, encoding='utf-8').read(), re.M) if i}
check(pot_ids == set(po_ids), 'pot 与 po 条目一致')

print('== 3. JSON / JS ==')
for f in sorted(glob.glob(REPO + '/**/*.json', recursive=True)):
    try:
        json.load(open(f, encoding='utf-8'))
    except Exception as e:
        check(False, 'JSON 解析失败 %s: %s' % (f, e))
        break
else:
    check(True, '所有 JSON 可解析')

if subprocess.call(['sh', '-c', 'command -v node >/dev/null']) == 0:
    r = subprocess.run(['node', '--check', VIEW], capture_output=True, text=True)
    check(r.returncode == 0, 'node --check 视图' + ('' if r.returncode == 0 else '  ' + r.stderr[:200]))
else:
    print('  skip node 未安装，跳过 JS 语法检查')

print('== 4. 与 LuCI 基础语言包的重叠（仅提示，不判失败）==')
# 本应用刻意复用 LuCI 基础库里的通用词（Interface / Gateway / IP address / MAC address /
# Username / Password / Enable / Cancel / Continue）。装了 LuCI 中文语言包时这些词由基础包
# 提供中文、其余由本应用翻译包提供 —— 属预期行为，这里只记录重叠项，不作为失败条件。
LUCI_BASE_PO = ('https://raw.githubusercontent.com/openwrt/luci/master/'
                'modules/luci-base/po/zh_Hans/base.po')
try:
    import urllib.request

    with urllib.request.urlopen(LUCI_BASE_PO, timeout=20) as resp:
        base_po = resp.read().decode('utf-8', 'replace')

    base_ids = {json.loads('"%s"' % i)
                for i in re.findall(r'^msgid "(.*)"$', base_po, re.M) if i}
    overlap = sorted(x for x in (code_ids & base_ids) if x)
    print('  提示：%d 项复用 LuCI 基础库（中文界面下由基础语言包翻译）：%s'
          % (len(overlap), overlap or '无'))
except Exception as exc:  # 离线时跳过
    print('  skip 取不到 LuCI 基础 po（%s），跳过' % exc)

print()
print('%d 项失败' % fails)
sys.exit(1 if fails else 0)

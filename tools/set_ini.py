"""Set simple KEY=VALUE lines in an ini (UTF-8, LF or CRLF preserved), for staging test builds."""
import io
import sys

path = sys.argv[1]
pairs = [a.split('=', 1) for a in sys.argv[2:]]
with io.open(path, 'r', encoding='utf-8-sig', newline='') as f:
    text = f.read()
newline = '\r\n' if '\r\n' in text else '\n'
lines = text.replace('\r\n', '\n').split('\n')
for key, value in pairs:
    hit = 0
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith(';') or '=' not in s:
            continue
        k = s.split('=', 1)[0].strip()
        if k == key:
            lines[i] = f'{key}={value}'
            hit += 1
    if hit == 0:
        lines.append(f'{key}={value}')
        print(f'  appended {key}={value}')
    else:
        print(f'  set {key}={value} ({hit} line(s))')
with io.open(path, 'w', encoding='utf-8', newline='') as f:
    f.write(newline.join(lines))

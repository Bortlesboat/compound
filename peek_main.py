with open(r'C:\Projects\AI\compound\src\main.c','r') as f:
    content = f.read()
# find app_init
idx = content.find('app_init')
if idx >= 0:
    # print surrounding
    start = content.rfind('\n', 0, max(0, idx-200))
    end = content.find('\n', idx+200)
    if start == -1: start = 0
    if end == -1: end = len(content)
    print(repr(content[start:end]))
else:
    print('not found')

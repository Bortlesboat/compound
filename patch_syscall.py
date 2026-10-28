import sys

path = sys.argv[1]
with open(path, 'r') as f:
    content = f.read()

# Add to enum
old = '''    HOS_SVC_CREATE_EVENT = 0x45,
    HOS_SVC_SLEEP_SYSTEM = 0x4D,
} hos_svc_t;'''
new = '''    HOS_SVC_CREATE_EVENT = 0x45,
    HOS_SVC_SLEEP_SYSTEM = 0x4D,
    /// Maps memory from one address to another (aliasing).
    HOS_SVC_MAP_MEMORY = 0x04,
    /// Unmaps previously mapped memory.
    HOS_SVC_UNMAP_MEMORY = 0x05,
} hos_svc_t;'''

if old not in content:
    print('NOT FOUND')
    sys.exit(1)

content = content.replace(old, new)
with open(path, 'w') as f:
    f.write(content)
print('ok')

with open('C:/Projects/AI/compound/src/core/hos/memory_layout.h', 'r') as f:
    content = f.read()

if content.count('#endif') > 1:
    idx = content.find('#endif')
    end = content.find('\n', idx)
    content_new = content[:end+1] + '\n\n/// Maps memory from src_addr to dst_addr with the given permissions (aliasing).\nerror_t hos_memory_layout_map_alias(hos_memory_layout_t *POUND_RESTRICT layout,\n                                    uint64_t src_addr,\n                                    uint64_t dst_addr,\n                                    uint64_t size,\n                                    uint32_t state,\n                                    uint32_t permission,\n                                    uint32_t attribute);\n\n/// Unmaps a region starting at base of size bytes, returning it to Free.\nerror_t hos_memory_layout_unmap(hos_memory_layout_t *POUND_RESTRICT layout,\n                                uint64_t base,\n                                uint64_t size);\n\n#endif // POUND_HOS_MEMORY_LAYOUT_H\n\n/*** end of file ***/\n'
    with open('C:/Projects/AI/compound/src/core/hos/memory_layout.h', 'w') as f:
        f.write(content_new)
    print('fixed')
else:
    # add if not present
    if 'hos_memory_layout_map_alias' not in content:
        content_new = content.replace('#endif // POUND_HOS_MEMORY_LAYOUT_H\n\n/*** end of file ***/', 
                                       '\n\n/// Maps memory from src_addr to dst_addr with the given permissions (aliasing).\nerror_t hos_memory_layout_map_alias(hos_memory_layout_t *POUND_RESTRICT layout,\n                                    uint64_t src_addr,\n                                    uint64_t dst_addr,\n                                    uint64_t size,\n                                    uint32_t state,\n                                    uint32_t permission,\n                                    uint32_t attribute);\n\n/// Unmaps a region starting at base of size bytes, returning it to Free.\nerror_t hos_memory_layout_unmap(hos_memory_layout_t *POUND_RESTRICT layout,\n                                uint64_t base,\n                                uint64_t size);\n\n#endif // POUND_HOS_MEMORY_LAYOUT_H\n\n/*** end of file ***/')
        with open('C:/Projects/AI/compound/src/core/hos/memory_layout.h', 'w') as f:
            f.write(content_new)
    print('done')

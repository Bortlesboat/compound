with open('C:/Projects/AI/compound/src/core/hos/memory_layout.h','r') as f:
    content = f.read()

start = content.find('#ifndef')
first_end = content.find('#endif', start)
# remove everything between first_end+len('#endif ...') and proper end
# just rebuild cleanly
clean = content[:start]
# copy up to hos_memory_layout_find declaration end
find_start = content.find('const guest_memory_region_t *hos_memory_layout_find')
# go to end of that declaration line
# find semicolon of that function
semi = content.find(';', find_start)
# now append
rest = '''

/// Maps memory from src_addr to dst_addr with the given permissions (aliasing).
error_t hos_memory_layout_map_alias(hos_memory_layout_t *POUND_RESTRICT layout,
                                    uint64_t src_addr,
                                    uint64_t dst_addr,
                                    uint64_t size,
                                    uint32_t state,
                                    uint32_t permission,
                                    uint32_t attribute);

/// Unmaps a region starting at base of size bytes, returning it to Free.
error_t hos_memory_layout_unmap(hos_memory_layout_t *POUND_RESTRICT layout,
                                uint64_t base,
                                uint64_t size);

#endif // POUND_HOS_MEMORY_LAYOUT_H

/*** end of file ***/
'''
new_content = content[:semi+1] + rest
with open('C:/Projects/AI/compound/src/core/hos/memory_layout.h','w') as f:
    f.write(new_content)
print('done')

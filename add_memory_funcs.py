code = open('C:/Projects/AI/compound/src/core/hos/memory_layout.c', 'r').read()

addition = r'''


error_t
hos_memory_layout_map_alias(hos_memory_layout_t *POUND_RESTRICT layout,
                            uint64_t src_addr,
                            uint64_t dst_addr,
                            uint64_t size,
                            uint32_t state,
                            uint32_t permission,
                            uint32_t attribute)
{
    if (layout == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: layout is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (layout->magic != HOS_MEMORY_LAYOUT_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: the layout is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (size == 0U)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias at 0x%llx: zero bytes.", (unsigned long long)dst_addr);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (((dst_addr % HOS_MEMORY_PAGE_SIZE) != 0U) || ((src_addr % HOS_MEMORY_PAGE_SIZE) != 0U) || ((size % HOS_MEMORY_PAGE_SIZE) != 0U))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: not page-aligned.");
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    if (dst_addr > (UINT64_MAX - size) || src_addr > (UINT64_MAX - size))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: size overflows.");
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    const uint64_t dst_end = dst_addr + size;
    const uint64_t src_end = src_addr + size;

    if ((dst_addr < layout->address_space_start) || (dst_end > layout->address_space_end))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: dst outside address space.");
        return POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS;
    }

    if ((src_addr < layout->address_space_start) || (src_end > layout->address_space_end))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: src outside address space.");
        return POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS;
    }

    const uint64_t heap_end = layout->heap_base + layout->heap_region_size;
    if ((dst_addr < heap_end) && (dst_end > layout->heap_base))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: overlaps heap region.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const int idx = layout_find_index(layout, dst_addr);
    if (idx < 0)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: dst not tiled.");
        return POUND_ERROR_CORRUPTED;
    }
    const guest_memory_region_t target = layout->regions[idx];
    if (target.state != (uint32_t)HOS_MEMORY_STATE_FREE)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: dst not Free.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }
    if ((dst_addr < target.base) || (dst_end > target.base + target.size))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: dst spans region boundary.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const guest_memory_region_t *src_region = hos_memory_layout_find(layout, src_addr);
    if (src_region == NULL || src_region->state == (uint32_t)HOS_MEMORY_STATE_FREE)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: src not mapped.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }
    if ((src_addr < src_region->base) || (src_end > src_region->base + src_region->size))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: src spans region boundary.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const uint64_t left_size  = dst_addr - target.base;
    const uint64_t right_size = (target.base + target.size) - dst_end;
    const uint32_t growth     = (uint32_t)(left_size > 0U ? 1U : 0U) + (uint32_t)(right_size > 0U ? 1U : 0U);

    if ((layout->region_count + growth) > HOS_MEMORY_LAYOUT_MAX_REGIONS)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map alias: table full.");
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    const uint32_t index_u32 = (uint32_t)idx;
    const uint32_t tail      = layout->region_count - (index_u32 + 1U);
    if (growth > 0U)
    {
        memmove(&layout->regions[index_u32 + 1U + growth], &layout->regions[index_u32 + 1U], (size_t)tail * sizeof(layout->regions[0]));
    }

    uint32_t at = index_u32;
    if (left_size > 0U)
    {
        layout->regions[at] = (guest_memory_region_t){ .base         = target.base,
                                                       .size         = left_size,
                                                       .permission   = 0U,
                                                       .state        = (uint32_t)HOS_MEMORY_STATE_FREE,
                                                       .attribute    = 0U,
                                                       .ipc_count    = 0U,
                                                       .device_count = 0U,
                                                       .alias_base   = 0U };
        at++;
    }

    layout->regions[at] = (guest_memory_region_t){ .base         = dst_addr,
                                                   .size         = size,
                                                   .permission   = permission,
                                                   .state        = state == 0 ? (uint32_t)HOS_MEMORY_STATE_ALIAS : state,
                                                   .attribute    = attribute,
                                                   .ipc_count    = src_region->ipc_count,
                                                   .device_count = src_region->device_count,
                                                   .alias_base   = src_addr };
    at++;

    if (right_size > 0U)
    {
        layout->regions[at] = (guest_memory_region_t){ .base         = dst_end,
                                                       .size         = right_size,
                                                       .permission   = 0U,
                                                       .state        = (uint32_t)HOS_MEMORY_STATE_FREE,
                                                       .attribute    = 0U,
                                                       .ipc_count    = 0U,
                                                       .device_count = 0U,
                                                       .alias_base   = 0U };
    }

    layout->region_count += growth;
    layout_publish_region_count(layout);
    return POUND_SUCCESS;
}

error_t
hos_memory_layout_unmap(hos_memory_layout_t *POUND_RESTRICT layout,
                        uint64_t base,
                        uint64_t size)
{
    if (layout == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout unmap: layout is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (layout->magic != HOS_MEMORY_LAYOUT_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout unmap: the layout is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (size == 0U)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout unmap at 0x%llx: zero bytes.", (unsigned long long)base);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (((base % HOS_MEMORY_PAGE_SIZE) != 0U) || ((size % HOS_MEMORY_PAGE_SIZE) != 0U))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout unmap: not page-aligned.");
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    if (base > (UINT64_MAX - size))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout unmap: size overflows.");
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    const uint64_t end = base + size;

    if ((base < layout->address_space_start) || (end > layout->address_space_end))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout unmap: outside address space.");
        return POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS;
    }

    const uint64_t heap_end = layout->heap_base + layout->heap_region_size;
    if ((base < heap_end) && (end > layout->heap_base))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout unmap: cannot unmap heap region.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const int first_idx = layout_find_index(layout, base);
    if (first_idx < 0)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout unmap: not tiled.");
        return POUND_ERROR_CORRUPTED;
    }

    const int last_idx = layout_find_index(layout, end - 1U);
    if (last_idx < 0)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout unmap: not tiled.");
        return POUND_ERROR_CORRUPTED;
    }

    for (int i = first_idx; i <= last_idx; i++)
    {
        if (layout->regions[i].state == (uint32_t)HOS_MEMORY_STATE_FREE)
        {
            POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout unmap: range contains Free region.");
            return POUND_ERROR_INVALID_ARGUMENT;
        }
    }

    uint32_t first = (uint32_t)first_idx;
    uint32_t last  = (uint32_t)last_idx;
    
    if ((layout->regions[first].base != base) || ((layout->regions[last].base + layout->regions[last].size) != end))
    {
        uint32_t splits = (uint32_t)(layout->regions[first].base != base) +
                          (uint32_t)((layout->regions[last].base + layout->regions[last].size) != end);
        if ((layout->region_count + splits) > HOS_MEMORY_LAYOUT_MAX_REGIONS)
        {
            return POUND_ERROR_ALLOCATION_FAILED;
        }
        uint32_t at = 0U;
        error_t error = layout_split_range(layout, base, end, first, last, &at);
        if (error != POUND_SUCCESS)
        {
            return error;
        }
        first = at;
        last = (uint32_t)layout_find_index(layout, end - 1U);
        if ((int)last < 0) return POUND_ERROR_CORRUPTED;
    }

    const uint32_t replaced = last - first + 1;
    const uint32_t tail     = layout->region_count - (last + 1);
    
    if (tail > 0U)
    {
        memmove(&layout->regions[first + 1], &layout->regions[last + 1], (size_t)tail * sizeof(layout->regions[0]));
    }
    
    layout->regions[first] = (guest_memory_region_t){ .base         = base,
                                                       .size         = size,
                                                       .permission   = 0U,
                                                       .state        = (uint32_t)HOS_MEMORY_STATE_FREE,
                                                       .attribute    = 0U,
                                                       .ipc_count    = 0U,
                                                       .device_count = 0U,
                                                       .alias_base   = 0U };
    
    layout->region_count = layout->region_count - replaced + 1;
    layout_publish_region_count(layout);
    return POUND_SUCCESS;
}
'''

with open('C:/Projects/AI/compound/src/core/hos/memory_layout.c', 'a') as f:
    f.write(addition)

print('done')
'
 2>&1 | Select-Object -Last 2
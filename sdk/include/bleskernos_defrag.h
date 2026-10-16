#ifndef BLESKERNOS_DEFRAG_H
#define BLESKERNOS_DEFRAG_H
#define BK_DEFRAG_CELLS 512U
#define BK_DEFRAG_IDLE 0U
#define BK_DEFRAG_RUNNING 1U
#define BK_DEFRAG_COMPLETE 2U
#define BK_DEFRAG_CANCELLED 3U
#define BK_DEFRAG_ERROR 4U
#define BK_DEFRAG_PENDING 0U
#define BK_DEFRAG_FREE 1U
#define BK_DEFRAG_PROCESSED 2U
#define BK_DEFRAG_FAILED 3U
#define BK_DEFRAG_RESERVED 4U
#define BK_DEFRAG_IO_ERROR 1U
#define BK_DEFRAG_INVALID_VOLUME 2U
#define BK_DEFRAG_CORRUPT_VOLUME 3U
#define BK_DEFRAG_NO_MEMORY 4U
#define BK_DEFRAG_BUSY 5U
#define BK_DEFRAG_CHANGED_FILE 6U
typedef struct {
    uint32_t state, error, owner, optimize;
    uint32_t total_blocks, free_blocks, block_size, data_start;
    uint32_t work_blocks, processed_blocks, verified_blocks;
    uint32_t files, fragmented_before, fragmented_after, moved_files, skipped_files;
    uint32_t current_inode, current_block, failed_block;
    char current_file[128];
    uint8_t cells[BK_DEFRAG_CELLS];
} bk_defrag_info_t;
#endif

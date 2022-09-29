#include "operations.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t open_lock;

int tfs_init() {
    state_init();

    /* Create root inode */
    int root = inode_create(T_DIRECTORY);
    if (root != ROOT_DIR_INUM) {
        return -1;
    }

    return 0;
}

int tfs_destroy() {
    state_destroy();
    return 0;
}

static bool valid_pathname(char const *name) {
    return name != NULL && strlen(name) > 1 && name[0] == '/';
}

int tfs_lookup(char const *name) {
    if (!valid_pathname(name)) {
        return -1;
    }

    // skip the initial '/' character
    name++;

    return find_in_dir(ROOT_DIR_INUM, name);
}

int tfs_open(char const *name, int flags) {
    int inum;
    size_t offset;

    pthread_mutex_lock(&open_lock);

    /* Checks if the path name is valid */
    if (!valid_pathname(name)) {
        return -1;
    }

    inum = tfs_lookup(name);
    if (inum >= 0) {
        /* The file already exists */
        inode_t *inode = inode_get(inum);
        if (inode == NULL) {
            pthread_mutex_unlock(&open_lock);
            return -1;
        }

        /* Trucate (if requested) */
        if (flags & TFS_O_TRUNC) {
            if (inode->i_size > 0) {
                for (int i = 0; i < 10; i++) {
                    if (data_block_free(inode->i_data_block[i]) == -1) {
                        pthread_mutex_unlock(&open_lock);
                        return -1;
                    }
                }
                inode->i_size = 0;
            }
        }
        /* Determine initial offset */
        if (flags & TFS_O_APPEND) {
            offset = inode->i_size;
        } else {
            offset = 0;
        }
    } else if (flags & TFS_O_CREAT) {
        /* The file doesn't exist; the flags specify that it should be created*/
        /* Create inode */
        inum = inode_create(T_FILE);
        if (inum == -1) {
            pthread_mutex_unlock(&open_lock);
            return -1;
        }
        /* Add entry in the root directory */
        if (add_dir_entry(ROOT_DIR_INUM, inum, name + 1) == -1) {
            inode_delete(inum);
            pthread_mutex_unlock(&open_lock);
            return -1;
        }
        offset = 0;
    } else {
        pthread_mutex_unlock(&open_lock);
        return -1;
    }

    /* Finally, add entry to the open file table and
     * return the corresponding handle */
    pthread_mutex_unlock(&open_lock);
    return add_to_open_file_table(inum, offset);

    /* Note: for simplification, if file was created with TFS_O_CREAT and there
     * is an error adding an entry to the open file table, the file is not
     * opened but it remains created */
}

int tfs_close(int sourcefile) {
    return remove_from_open_file_table(sourcefile);
}

ssize_t tfs_write(int sourcefile, void const *buffer, size_t to_write) {
    size_t total_written = 0;
    size_t write_one_block = 0;
    size_t to_write_backup = to_write;

    open_file_entry_t *file = get_open_file_entry(sourcefile);

    if (file == NULL) {
        return -1;
    }

    /* From the open file table entry, we get the inode */
    inode_t *inode = inode_get(file->of_inumber);
    if (inode == NULL) {
        return -1;
    }
    // Variable that saves the block position
    size_t block_pos = (file->of_offset / BLOCK_SIZE);
    // Variable that saves the actual position in bytes on the block
    size_t actual_pos = (file->of_offset % BLOCK_SIZE);
    // Pointer to a indirect block
    int *pointers_block;
    pthread_rwlock_wrlock(&inode->inode);
    while (to_write > 0) {
        block_pos = (file->of_offset / BLOCK_SIZE);
        actual_pos = (file->of_offset % BLOCK_SIZE);
        write_one_block = to_write;
        if (inode->i_size == 0) {
            inode->i_data_block[0] = data_block_alloc();
        }
        /* Determine how many bytes to write */
        if (write_one_block + file->of_offset > (block_pos + 1) * BLOCK_SIZE) {
            write_one_block = ((block_pos + 1) * BLOCK_SIZE) - file->of_offset;
        }
        if (inode->i_data_block[block_pos] == -1) {
            inode->i_data_block[block_pos] = data_block_alloc();
            if (inode->i_data_block[block_pos] == -1) {
                pthread_rwlock_unlock(&inode->inode);
                return -1;
            }
        }
        void *block;
        if (block_pos < 10) { // Get the direct blocks
            block = data_block_get(inode->i_data_block[block_pos]);
        } else if (block_pos > 9) { // In case of is a indirect block
            if (inode->i_data_block[10] == -1) {
                inode->i_data_block[10] = data_block_alloc();
            }
            pointers_block = (int *)data_block_get(inode->i_data_block[10]);
            if (actual_pos % BLOCK_SIZE == 0) {
                pointers_block[block_pos - 10] = data_block_alloc();
            }
            block = data_block_get(pointers_block[block_pos - 10]);
        }

        if (block == NULL) {
            pthread_rwlock_unlock(&inode->inode);
            return -1;
        }
        if (to_write > BLOCK_SIZE - actual_pos) {
            write_one_block = BLOCK_SIZE - actual_pos;
        } else if (to_write <= BLOCK_SIZE) {
            write_one_block = to_write;
        }
        /* Perform the actual write */
        memcpy(block + actual_pos, buffer + total_written, write_one_block);
        total_written += write_one_block;
        /* The offset associated with the file handle incremented accordingly */
        to_write -= write_one_block;
        file->of_offset += write_one_block;
        if (file->of_offset > inode->i_size) {
            inode->i_size = file->of_offset;
        }
    }
    pthread_rwlock_unlock(&inode->inode);
    return (ssize_t)to_write_backup;
}

ssize_t tfs_read(int sourcefile, void *buffer, size_t len) {

    open_file_entry_t *file = get_open_file_entry(sourcefile);

    if (file == NULL) {
        return -1;
    }

    /* From the open file table entry, we get the inode */
    inode_t *inode = inode_get(file->of_inumber);
    if (inode == NULL) {
        return -1;
    }

    size_t to_read = inode->i_size - file->of_offset;
    if (to_read > len) {
        to_read = len;
    }

    if (file->of_offset + to_read > inode->i_size) {
        return -1;
    }

    // Number of bytes readed
    size_t readed_bytes = to_read;
    // Variable that saves what can be read in the actual block
    size_t read_one_block = 0;
    // Variable that saves the number of blocks that are going to be read
    size_t block_pos = 0;
    // Variable that saves the actual position in a block in bytes
    size_t actual_pos = 0;
    // Pointer to a indirect block
    int(*pointers_block) = 0;

    pthread_rwlock_rdlock(&inode->inode);
    while (to_read > 0) {

        read_one_block = to_read;
        block_pos = file->of_offset / BLOCK_SIZE;
        actual_pos = file->of_offset % BLOCK_SIZE;

        if (read_one_block + file->of_offset > (block_pos + 1) * BLOCK_SIZE) {
            read_one_block = ((block_pos + 1) * BLOCK_SIZE) - file->of_offset;
        }
        void *block = NULL;
        if (block_pos < 10) {
            block = data_block_get(inode->i_data_block[block_pos]);
        } else {
            pointers_block = (int *)data_block_get(inode->i_data_block[10]);
            block = data_block_get(pointers_block[block_pos - 10]);
        }

        if (block == NULL) {
            pthread_rwlock_unlock(&inode->inode);
            return -1;
        }

        if (to_read > BLOCK_SIZE - actual_pos) {
            read_one_block = BLOCK_SIZE - actual_pos;
        }

        /* Perform the actual read */

        memcpy(buffer, block + actual_pos, read_one_block);
        to_read -= read_one_block;
        buffer += read_one_block;

        /* The offset associated with the file handle is
         * incremented accordingly */
        file->of_offset += read_one_block;
    }
    pthread_rwlock_unlock(&inode->inode);
    return (ssize_t)readed_bytes;
}

int tfs_copy_to_external_fs(char const *source_path, char const *dest_path) {
    int sourcefile =
        tfs_open(source_path, 0); /*Opens the file in the tfs system*/
    FILE *newfile =
        fopen(dest_path, "ab+"); /*Opens an existing file from the PC or Creates
                                    a new one if does not exist*/
    char buffer[1024];
    size_t len = sizeof(buffer);

    if (sourcefile == -1 || newfile == NULL) {
        return -1;
    }

    while (tfs_read(sourcefile, buffer, len)) {
        fwrite(buffer, sizeof(char), sizeof(buffer), newfile);
        memset(buffer, 0, sizeof(buffer));
    }

    tfs_close(sourcefile);
    fclose(newfile);

    return 0;
}

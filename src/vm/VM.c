/*
 * ===========================================================================
 * PROJET ARCHITECTURE DES ORDINATEURS
 * ===========================================================================
 * File: VM.c
 * Author: Thomas PRÉVOST @ ENSTA Bretagne
 * ---
 * Description:
 * ---
 * Virtual-Machine capable of executing binary code.
 * Binaries placed under /assembled can be generated w/ assemble.py from ASM files.
 */


// https://www.jmeiners.com/lc3-vm/index.html
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
/* unix-systems only */
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>


/* Memory storage */
#define MEM_MAX (1<<16)
u_int16_t mem[MEM_MAX];

/* Registers */
enum{
    R_R0 = 0,
    R_R1,
    R_R2,
    R_R3,
    R_R4,
    R_R5,
    R_R6,
    R_R7,
    R_PC,
    R_COND,
    R_COUNT
};

/* register storage */
u_int16_t reg[R_COUNT];

/* instructions */
enum{
    OP_BR = 0, /* branch */
    OP_ADD,    /* add  */
    OP_LD,     /* load */
    OP_ST,     /* store */
    OP_JSR,    /* jump register */
    OP_AND,    /* bitwise and */
    OP_LDR,    /* load register */
    OP_STR,    /* store register */
    OP_RTI,    /* unused */
    OP_NOT,    /* bitwise not */
    OP_LDI,    /* load indirect */
    OP_STI,    /* store indirect */
    OP_JMP,    /* jump */
    OP_RES,    /* reserved (unused) */
    OP_LEA,    /* load effective address */
    OP_TRAP    /* execute trap */
};

/* condition flags */
enum{
    FL_POS = 1 << 0, /* P */
    FL_ZRO = 1 << 1, /* Z */
    FL_NEG = 1 << 2  /* N */
};

/* trap codes */
enum{
    TRAP_GETC = 0x20, /* get character from keyboard, not echoed onto the terminal */
    TRAP_OUT  = 0x21, /* output a character */
    TRAP_PUTS = 0x22, /* output a word string */
    TRAP_IN   = 0x23, /* get character from keyboard, echoed onto the terminal */
    TRAP_PUTSP= 0x24, /* output a byte string */
    TRAP_HALT = 0x25  /* halt the program */
};

/* memory mapped registers */
enum{
    MR_KBSR = 0xFE00, /* keyboard status */
    MR_KBDR = 0xFE02  /* keyboard data */
};

/* input buffer */
struct termios original_tio;
void disable_input_buffering()
{
    tcgetattr(STDIN_FILENO, &original_tio);
    struct termios new_tio = original_tio;
    new_tio.c_lflag &= ~ICANON & ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_input_buffering()
{
    tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

uint16_t check_key(){
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout) != 0;
}


/* update flags */
void update_flags(uint16_t r){
    if (reg[r] == 0){
        reg[R_COND] = FL_ZRO; // 0
    } else if (reg[r] >> 15){ /* a 1 in the left-most bit indicates negative */
        reg[R_COND] = FL_NEG; // negative nbr
    } else {
        reg[R_COND] = FL_POS; // pos nbr
    }
}

/* loading programs */
void read_image_file(FILE* file){
    /* the origin tells us where in memory to place the image */
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = ntohs(origin);

    /* we know the maximum file size, so we only need one fread */
    uint16_t max_read = (MEM_MAX - origin);
    uint16_t* p = mem + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

    /* swap to little endian */
    while (read-- > 0){
        *p = ntohs(*p);
        ++p;
    }
}

uint16_t swap16(uint16_t x){
    return (x << 8) | (x >> 8);
}

int read_image(const char* image_path){
    FILE* file = fopen(image_path, "rb");
    if (!file) { return 0; };
    read_image_file(file);
    fclose(file);
    return 1;
}

/* memory access */
void mem_write(uint16_t address, uint16_t val){
    mem[address] = val;
}

uint16_t mem_read(uint16_t address){
    if (address == MR_KBSR){
        if (check_key()){
            mem[MR_KBSR] = (1 << 15);
            mem[MR_KBDR] = getchar();
        } else {
            mem[MR_KBSR] = 0;
        }
    }
    return mem[address];
}

/* handle interrupts */
void handle_interrupt(int signal) {
    restore_input_buffering();
    printf("\n");
    exit(-2);
}

uint16_t sign_extend(uint16_t x, int bit_count){
    if ((x >> (bit_count - 1)) & 1){
        x |= (0xFFFF << bit_count);
    }
    return x;
}

int main(int argc, char *argv[]){
    /* loading arguments */
    if(argc < 2){
        printf("VM [image-file1] ...\n");
        exit(2);
    }

    for (int j = 1; j < argc ; ++j){
        if (!read_image(argv[j])){
            printf("failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }

    signal(SIGINT, handle_interrupt);
    disable_input_buffering();

    /* one condition flag shall be given ; set Z flag */
    reg[R_COND] = FL_ZRO;

    /* set PC to starting position */
    /* 0x3000 is the default */
    enum { PC_START = 0x3000 };
    reg[R_PC] = PC_START;

    int running=1;
    while(running){
        /* FETCH */
        u_int16_t instr = mem_read(reg[R_PC]++);
        u_int16_t op = instr >> 12;

        switch(op){
            case OP_ADD:
            {
                /* destination register (DR) */
                u_int16_t r0 = (instr >> 9) & 0x7;
                /* first operand (SR1) */
                u_int16_t r1 = (instr >> 6) & 0x7;
                /* whether we are in immediate mode */
                u_int16_t imm_flag = (instr >> 5) & 0x1;

                if(imm_flag){
                    u_int16_t imm5 = sign_extend(instr & 0x1F, 5);
                    reg[r0] = reg[r1] + imm5;
                }else{
                    u_int16_t r2 = instr & 0x7;
                    reg[r0] = reg[r1] + reg[r2];
                }

                update_flags(r0);
            }
                break;
            case OP_AND:
            {
                uint16_t r0 = (instr >> 9) & 0x7;
                uint16_t r1 = (instr >> 6) & 0x7;
                uint16_t imm_flag = (instr >> 5) & 0x1;

                if (imm_flag){
                    uint16_t imm5 = sign_extend(instr & 0x1F, 5);
                    reg[r0] = reg[r1] & imm5;
                } else {
                    uint16_t r2 = instr & 0x7;
                    reg[r0] = reg[r1] & reg[r2];
                }
                update_flags(r0);
            }
                break;
            case OP_NOT:
            {
                uint16_t r0 = (instr >> 9) & 0x7;
                uint16_t r1 = (instr >> 6) & 0x7;
                reg[r0] = ~reg[r1];
                update_flags(r0);
            }
                break;
            case OP_BR:
            {
                uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                uint16_t cond_flag = (instr >> 9) & 0x7;
                if (cond_flag & reg[R_COND]){
                    reg[R_PC] += pc_offset;
                }
            }
                break;
            case OP_JMP:
                /* jump */
            {
                uint16_t r1 = (instr >> 6) & 0x7;
                reg[R_PC] = reg[r1];
            }
                break;
            case OP_JSR:
                /* jump register */
            {
                uint16_t long_flag = (instr >> 11) & 1;
                reg[R_R7] = reg[R_PC];
                if(long_flag){
                    uint16_t long_pc_offset = sign_extend(instr & 0x7FF, 11);
                    reg[R_PC] += long_pc_offset;
                } else {
                    uint16_t r1 = (instr >> 6) & 0x7;
                    reg[R_PC] = reg[r1];
                }
            }
                break;
            case OP_LD:
                /* load */
            {
                uint16_t r0 = (instr >> 9) & 0x7;
                uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                reg[r0] = mem_read(reg[R_PC] + pc_offset);
                update_flags(r0);
            }
                break;
            case OP_LDI:
                /* load indirect */
            {
                /* destination register (DR) */
                uint16_t r0 = (instr >> 9) & 0x7;
                /* PCoffset 9 */
                uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                /* add pc_offset to the current PC, look at that memory location to get the final address */
                reg[r0] = mem_read(mem_read(reg[R_PC] + pc_offset));
                update_flags(r0);
            }
                break;
            case OP_LDR:
                /* load register */
            {
                uint16_t r0 = (instr >> 9) & 0x7;
                uint16_t r1 = (instr >> 6) & 0x7;
                uint16_t offset = sign_extend(instr & 0x3F, 6);
                reg[r0] = mem_read(reg[r1] + offset);
                update_flags(r0);
            }
                break;
            case OP_LEA:
                /* lod effective address */
            {
                uint16_t r0 = (instr >> 9) & 0x7;
                uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                reg[r0] = reg[R_PC] + pc_offset;
                update_flags(r0);
            }
                break;
            case OP_ST:
                /* store */
            {
                uint16_t r0 = (instr >> 9) & 0x7;
                uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                mem_write(reg[R_PC] + pc_offset, reg[r0]);
            }
                break;
            case OP_STI:
                /* store indirect */
            {
                uint16_t r0 = (instr >> 9) & 0x7;
                uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                mem_write(mem_read(reg[R_PC] + pc_offset), reg[r0]);
            }
                break;
            case OP_STR:
                /* store register */
            {
                uint16_t r0 = (instr >> 9) & 0x7;
                uint16_t r1 = (instr >> 6) & 0x7;
                uint16_t offset = sign_extend(instr & 0x3F, 6);
                mem_write(reg[r1] + offset, reg[r0]);
            }
                break;
            case OP_TRAP:
                /* TRAP */
            {
                reg[R_R7] = reg[R_PC];
                switch (instr & 0xFF) {
                    case TRAP_GETC:
                        reg[R_R0] = (uint16_t)getchar();
                        break;
                    case TRAP_OUT:
                        putc((char)reg[R_R0], stdout);
                        fflush(stdout);
                        break;
                    case TRAP_PUTS:
                    {
                        /* one char per word */
                        uint16_t* c = mem + reg[R_R0];
                        while (*c){
                            putc((char)*c, stdout);
                            ++c;
                        }
                        fflush(stdout);
                    }
                        break;
                    case TRAP_IN:
                    {
                        printf("Enter a character: ");
                        char c = getchar();
                        putc(c, stdout);
                        reg[R_R0] = (uint16_t)c;
                    }
                        break;
                    case TRAP_PUTSP:
                    {
                        /* one char per byte (two bytes per word) */
                        uint16_t* c = mem + reg[R_R0];
                        while (*c){
                            char char1 = (*c) & 0xFF;
                            putc(char1, stdout);
                            char char2 = (*c) >> 8;
                            if (char2) putc(char2, stdout);
                            ++c;
                        }
                        fflush(stdout);
                    }
                        break;
                    case TRAP_HALT:
                        puts("HALT");
                        fflush(stdout);
                        running = 0;
                        break;
                }
            }
                break;
            case OP_RES:
            case OP_RTI:
            default:
                abort();
                break;
        }
        restore_input_buffering();
    }
}






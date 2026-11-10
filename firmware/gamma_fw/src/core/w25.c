/* w25.c — см. w25.h. */
#include "w25.h"

#define CMD_WREN   0x06u
#define CMD_RDSR1  0x05u
#define CMD_READ   0x03u
#define CMD_PP     0x02u
#define CMD_SE4K   0x20u
#define CMD_BE64K  0xD8u
#define CMD_JEDEC  0x9Fu
#define CMD_PD     0xB9u
#define CMD_RPD    0xABu

static void cmd1(const w25_port_t *p, uint8_t c)
{
    p->select(true);
    p->xfer(&c, NULL, 1);
    p->select(false);
}

static void cmd_addr(const w25_port_t *p, uint8_t c, uint32_t a)
{
    uint8_t h[4] = { c, (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a };
    p->xfer(h, NULL, 4);
}

void w25_wake(const w25_port_t *p)
{
    cmd1(p, CMD_RPD);
    if (p->delay_us) p->delay_us(5);          /* tRES1 ≤ 3 мкс */
}

void w25_sleep(const w25_port_t *p)
{
    cmd1(p, CMD_PD);
    if (p->delay_us) p->delay_us(5);          /* tDP ≤ 3 мкс */
}

void w25_jedec(const w25_port_t *p, uint8_t id[3])
{
    uint8_t c = CMD_JEDEC;
    p->select(true);
    p->xfer(&c, NULL, 1);
    p->xfer(NULL, id, 3);
    p->select(false);
}

bool w25_busy(const w25_port_t *p)
{
    uint8_t c = CMD_RDSR1, s = 0;
    p->select(true);
    p->xfer(&c, NULL, 1);
    p->xfer(NULL, &s, 1);
    p->select(false);
    return (s & 1u) != 0;
}

void w25_read(const w25_port_t *p, uint32_t addr, void *buf, size_t n)
{
    p->select(true);
    cmd_addr(p, CMD_READ, addr);
    p->xfer(NULL, (uint8_t *)buf, n);
    p->select(false);
}

void w25_program(const w25_port_t *p, uint32_t addr, const void *buf, size_t n)
{
    cmd1(p, CMD_WREN);
    p->select(true);
    cmd_addr(p, CMD_PP, addr);
    p->xfer((const uint8_t *)buf, NULL, n);
    p->select(false);
}

void w25_erase4k(const w25_port_t *p, uint32_t addr)
{
    cmd1(p, CMD_WREN);
    p->select(true);
    cmd_addr(p, CMD_SE4K, addr & ~(W25_SECTOR - 1u));
    p->select(false);
}

void w25_erase64k(const w25_port_t *p, uint32_t addr)
{
    cmd1(p, CMD_WREN);
    p->select(true);
    cmd_addr(p, CMD_BE64K, addr & ~(W25_BLOCK - 1u));
    p->select(false);
}

#define GPU_MMIO_BASE 0x1F802000
#define REG_WIDTH (*(volatile unsigned short *)(GPU_MMIO_BASE + 0x00))
#define REG_HEIGHT (*(volatile unsigned short *)(GPU_MMIO_BASE + 0x02))
#define REG_PITCH (*(volatile unsigned short *)(GPU_MMIO_BASE + 0x04))
#define REG_BPP (*(volatile unsigned char *)(GPU_MMIO_BASE + 0x06))
#define REG_FBADDR (*(volatile unsigned int *)(GPU_MMIO_BASE + 0x08))
#define REG_CMD16 (*(volatile unsigned short *)(GPU_MMIO_BASE + 0x10))
#define REG_PARAM16 (*(volatile unsigned short *)(GPU_MMIO_BASE + 0x12))

static inline void gpu_param_u8(int i, unsigned char v) {
  *(volatile unsigned char *)(GPU_MMIO_BASE + 0x12 + i) = v;
}
static inline void gpu_cmd_u8(int i, unsigned char v) {
  *(volatile unsigned char *)(GPU_MMIO_BASE + 0x10 + i) = v;
}
static inline void gpu_param_u16(unsigned short v) {
  gpu_param_u8(0, v & 0xFF);
  gpu_param_u8(1, v >> 8);
}
static inline void gpu_param_u32(unsigned int v) {
  gpu_param_u16(v & 0xFFFF);
  gpu_param_u16(v >> 16);
}
static inline void gpu_cmd(unsigned short c) {
  gpu_cmd_u8(0, c & 0xFF);
  gpu_cmd_u8(1, c >> 8);
}

void _start() {
  REG_WIDTH = 320;
  REG_HEIGHT = 200;
  REG_PITCH = 320;
  REG_BPP = 32;
  REG_FBADDR = 0;

  unsigned int c00 = 0xFF3030C0;
  unsigned int c10 = 0xFFC050C0;
  unsigned int c01 = 0xFF30C080;
  unsigned int c11 = 0xFFF5D060;

  gpu_param_u32(c00);
  gpu_param_u32(c10);
  gpu_param_u32(c01);
  gpu_param_u32(c11);
  gpu_cmd(0x000B); // GRAD_XY

  while (1) {
  }
}

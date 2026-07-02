// fputest.c — aritmética de punto flotante real (ver
// aiz32mips_core::cpu::Instruction::Cop1): antes de esto, cualquier
// operación float/double de un programa de usuario compilaba a
// instrucciones COP1 reales (add.s, c.lt.s, bc1t, etc.) que el emulador
// sólo reconocía a medias — MFC1/MTC1 sí, pero la aritmética/comparación/
// conversión de verdad caía en "no implementado" y devolvía basura
// silenciosa. Sin libm (freestanding): sólo +- * / , casts y comparación,
// que ya alcanza para ejercitar ADD/SUB/MUL/DIV/CVT/TRUNC/C.cond/BC1 tanto
// en S (float) como D (double).
#include "libc/stdio.h"
#include "libc/stdlib.h"

// volatile: sin esto clang -O2 calcula todo en tiempo de compilación (son
// todas constantes) y no queda ni una instrucción COP1 real en el
// binario — la prueba "pasaba" sin ejercitar nada. Forzando la carga real
// de a/b/da/db/ia/fb desde memoria, el resto de las cuentas sí las hace en
// runtime con add.s/sub.s/mul.s/div.s/c.cond.s/bc1t/trunc.w.s/etc de
// verdad.
void _start(void) {
    int ok = 1;

    volatile float va = 7.5f;
    volatile float vb = 2.5f;
    float a = va;
    float b = vb;
    if ((int)(a + b) != 10) {
        printf("float add fallo\n");
        ok = 0;
    }
    if ((int)(a - b) != 5) {
        printf("float sub fallo\n");
        ok = 0;
    }
    if ((int)(a * b) != 18) { // 7.5*2.5=18.75, trunca a 18
        printf("float mul fallo\n");
        ok = 0;
    }
    if ((int)(a / b) != 3) {
        printf("float div fallo\n");
        ok = 0;
    }

    volatile double vda = 1234.5;
    volatile double vdb = 1000.25;
    double da = vda;
    double db = vdb;
    if ((int)(da + db) != 2234) {
        printf("double add fallo\n");
        ok = 0;
    }
    if ((int)(da - db) != 234) {
        printf("double sub fallo\n");
        ok = 0;
    }

    volatile int via = 42;
    int ia = via;
    float fa = (float)ia; // CVT.S.W
    if ((int)fa != 42) {
        printf("int->float fallo\n");
        ok = 0;
    }

    volatile float vfb = -9.0f;
    float fb = vfb;
    int ib = (int)fb; // TRUNC.W.S
    if (ib != -9) {
        printf("float->int fallo\n");
        ok = 0;
    }

    if (!(a > b)) { // C.cond.S + BC1
        printf("float compare > fallo\n");
        ok = 0;
    }
    if (!(b < a)) {
        printf("float compare < fallo\n");
        ok = 0;
    }
    if (a == b) {
        printf("float compare == fallo\n");
        ok = 0;
    }

    if (ok) {
        printf("fputest: aritmetica de punto flotante ok\n");
    }
    exit(0);
}

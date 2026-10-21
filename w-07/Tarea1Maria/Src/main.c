// Tarea 1 - Taller V: contador con dos photo-interruptores y un display de 7 segmentos de 4 digitos.
// Todo esta escrito bare-metal, configurando los registros directamente con las definiciones de CMSIS.
//
// HARDWARE QUE USE
// - Tarjeta NUCLEO-F411RE (microcontrolador STM32F411RE, Cortex-M4) montada en la board tactica.
//   Trabajo con el reloj interno HSI de 16 MHz que trae por defecto, no configure el PLL.
// - Boton de reset: uso el boton RESET de la tarjeta, que va directo al pin NRST del micro.
//   Se puede presionar en cualquier momento y el programa arranca de nuevo desde el main.
// - LED de estado: el LED de la board conectado a PH1. Es el blinky que me muestra que el micro esta vivo.
// - Display 5641-BSR: 4 digitos de 7 segmentos, anodo comun, color rojo.
//   Los 7 segmentos (catodos) van cada uno con su resistencia de 330 Ohm a un pin del micro.
//   Los 4 anodos comunes los manejo con transistores PNP 2N3906 (emisor a 3,3 V, base con 1,5 kOhm),
//   porque un pin solo no aguanta la corriente de todo un digito prendido.
//   Abajo esta la tabla con cada pin del display y el pin del micro al que lo conecte.
// - Dos photo-interruptores (fotocompuertas): cada uno tiene un LED infrarrojo y un fototransistor,
//   cuando algo tapa el haz la salida cambia de nivel. La salida ya tiene su resistencia en la protoboard,
//   por eso las entradas del micro no llevan pull-up ni pull-down interno.
//     exit1 -> PC3 -> EXTI3, reconoce el flanco de SUBIDA y suma 1 al numero.
//     exit2 -> PC1 -> EXTI1, reconoce el flanco de BAJADA y resta 1 al numero.u
//
// COMO FUNCIONA EL SISTEMA
// Uso 5 interrupciones, todas con la misma prioridad (la de fabrica), asi ninguna corta a otra:
// - TIM2 (cada 1 ms): refresco del display. Prendo un solo digito a la vez y voy rotando,
//   en 4 ms paso por los 4 digitos (250 Hz). Por el efecto POV (persistencia de la vision) el ojo
//   los ve todos prendidos al tiempo. El profesor pide minimo 30 refrescos por segundo
//   (33 ms por vuelta) y yo estoy muy por encima. Si quiero cambiarlo solo cambio TIEMPO_DIGITO_MS.
//   Para que no haya fantasma, antes de cambiar los segmentos apago todos los digitos.
// - TIM3 (cada 250 ms): blinky del LED de estado. Cada 250 ms cambio el LED, o sea que parpadea
//   a 2 Hz (250 ms prendido y 250 ms apagado). Este timer no hace nada mas.
// - EXTI3 (exit1, PC3): cada flanco de subida suma 1 al contador.
// - EXTI1 (exit2, PC1): cada flanco de bajada resta 1 al contador.
// - SysTick (cada 1 ms): solo cuenta milisegundos en ticks_ms. Lo uso para el antirrebote:
//   si un photo-interruptor da dos flancos con menos de ANTIRREBOTE_MS de diferencia, el segundo no lo cuento.
// Callbacks: cada rutina de interrupcion solo borra su bandera y llama a su funcion:
//   TIM2_IRQHandler -> actualizar_display(), TIM3_IRQHandler -> led_estado(),
//   EXTI3_IRQHandler y EXTI1_IRQHandler hacen el conteo con su antirrebote.
// Maquina de estados (en el while(1) del main):
//   ESTADO_INICIO     -> dejo el contador en 0 y el display mostrando 0000.
//   ESTADO_ESPERA     -> no hago nada hasta que una EXTI me avise con bandera_cambio que cambio el contador.
//   ESTADO_ACTUALIZAR -> leo el contador, lo separo en millares, centenas, decenas y unidades en digitos[]
//                        y vuelvo a ESTADO_ESPERA. TIM2 muestra el numero nuevo solo en la siguiente vuelta.
// Cada parte va por separado y solo se comunican por variables en RAM:
//   las EXTI escriben contador y bandera_cambio, la maquina de estados las lee y llena digitos[],
//   TIM2 solo lee digitos[], el SysTick solo escribe ticks_ms y las EXTI lo leen.
// El contador es de 16 bits sin signo: si baja de 0 se va a 65535 y si pasa de 65535 vuelve a 0.
//   Como solo tengo 4 digitos, 65535 se ve como 5535.

#include <stdint.h>
#include "stm32f411xe.h"
#include "stm32f4xx.h"

// Asi conecte el display (saque los pines del datasheet del 5641-BSR):
// pin 1  -> E    -> PD2
// pin 2  -> D    -> PC11
// pin 3  -> DP   -> no lo conecte
// pin 4  -> C    -> PC13
// pin 5  -> G    -> PB7
// pin 6  -> DIG4 -> PC12 (unidades)
// pin 7  -> B    -> PA12
// pin 8  -> DIG3 -> PC5  (decenas)
// pin 9  -> DIG2 -> PC6  (centenas)
// pin 10 -> F    -> PA11
// pin 11 -> A    -> PB12
// pin 12 -> DIG1 -> PC10 (millares)
// Le puse 330 Ohm a cada uno de los 7 segmentos (con 3,3 V son unos 4 mA por segmento),
// 1,5 kOhm en la base de los 2N3906 y el emisor lo conecte a 3,3 V

// Guardo el puerto y la mascara del pin en el ODR juntos para poder recorrer los segmentos con un for.
// La mascara es la del registro ODR (GPIO_ODR_ODy), la que tiene un 1 solo en el bit de ese pin
typedef struct {
    GPIO_TypeDef *puerto;
    uint32_t mascara;
} PinGPIO;

// Los segmentos de la a a la g en orden. Si cambio un cable tengo que cambiarlo aqui y en init_GPIO
static const PinGPIO pines_segmentos[7] = {
    {GPIOB, GPIO_ODR_OD12},  // a -> PB12
    {GPIOA, GPIO_ODR_OD12},  // b -> PA12
    {GPIOC, GPIO_ODR_OD13},  // c -> PC13
    {GPIOC, GPIO_ODR_OD11},  // d -> PC11
    {GPIOD, GPIO_ODR_OD2},   // e -> PD2
    {GPIOA, GPIO_ODR_OD11},  // f -> PA11
    {GPIOB, GPIO_ODR_OD7},   // g -> PB7
};

// Los anodos de cada digito, de izquierda a derecha
static const PinGPIO pines_digitos[4] = {
    {GPIOC, GPIO_ODR_OD10},  // DIG1 -> PC10
    {GPIOC, GPIO_ODR_OD6},   // DIG2 -> PC6
    {GPIOC, GPIO_ODR_OD5},   // DIG3 -> PC5
    {GPIOC, GPIO_ODR_OD12},  // DIG4 -> PC12
};

// Como el display es de anodo comun, el segmento prende con 0.
// Los digitos los manejo con 2N3906 (PNP), que conducen cuando la base esta en 0,
// entonces el digito tambien prende con 0.
#define SEGMENTO_ACTIVO_ALTO   0
#define DIGITO_ACTIVO_ALTO     0

// Lo que dura prendido cada digito (en ms). 4 digitos por este tiempo es una vuelta completa.
// Tengo que cuidar que 4 x TIEMPO_DIGITO_MS no pase de 33 ms (minimo 30 refrescos por segundo)
#define TIEMPO_DIGITO_MS       1

// Tiempo minimo entre dos conteos de la misma fotocompuerta.
// Si me llegan flancos mas seguidos que esto los tomo como rebote y no los cuento.
#define ANTIRREBOTE_MS         20

// Las puse volatile porque las cambian las interrupciones
volatile uint16_t contador = 0;
volatile uint8_t bandera_cambio = 0;       // la EXTI la pone en 1 para avisarle al main que cambio el contador
volatile uint8_t digitos[4] = {0, 0, 0, 0}; // lo que TIM2 va mostrando en el display
volatile uint8_t digito_activo = 0;        // el digito que esta prendido en este momento (0..3)
volatile uint32_t ticks_ms = 0;            // los milisegundos desde que arranco, los va sumando el SysTick
volatile uint32_t ultimo_sensor1 = 0;      // en que ticks_ms conte por ultima vez exit1 (PC3)
volatile uint32_t ultimo_sensor2 = 0;      // en que ticks_ms conte por ultima vez exit2 (PC1)

typedef enum {
    ESTADO_INICIO,
    ESTADO_ESPERA,
    ESTADO_ACTUALIZAR
} Estado;

Estado estado_actual = ESTADO_INICIO;

// Un bit por segmento: el bit0 es a ... el bit6 es g, y 1 es prendido
static const uint8_t patrones[10] = {
    0x3F, // 0
    0x06, // 1
    0x5B, // 2
    0x4F, // 3
    0x66, // 4
    0x6D, // 5
    0x7D, // 6
    0x07, // 7
    0x7F, // 8
    0x6F, // 9
};

void init_GPIO(void);
void init_SysTick(void);
void init_EXTI(void);
void init_TIM2(void);
void init_TIM3(void);

void led_estado(void);
void dividir_numero(uint16_t valor);
void dibujar_numero(uint8_t digito);
void apagar_display(void);
void actualizar_display(void);

static void escribir_pin(const PinGPIO *p, uint8_t encendido, uint8_t activo_alto);

int main(void)
{
    uint16_t copia_contador;

    // Configuro todos los perifericos una sola vez antes de entrar al while
    init_GPIO();
    init_SysTick();     // lo prendo antes que las EXTI porque el antirrebote usa ticks_ms
    init_EXTI();
    init_TIM2();
    init_TIM3();

    while (1) {

        switch (estado_actual) {

            case ESTADO_INICIO:
                // Dejo el contador en 0 y el display mostrando 0000 para arrancar
                contador = 0;
                bandera_cambio = 0;
                dividir_numero(0);


                estado_actual = ESTADO_ESPERA;
                break;

            case ESTADO_ESPERA:
                // Me quedo aqui hasta que alguna fotocompuerta cambie el contador
                if (bandera_cambio) {
                    estado_actual = ESTADO_ACTUALIZAR;
                }
                break;

            case ESTADO_ACTUALIZAR:
                // Primero bajo la bandera y despues leo el contador, asi si entra otra
                // interrupcion en medio la bandera vuelve a 1 y no se me pierde la cuenta
                bandera_cambio = 0;
                copia_contador = contador;

                dividir_numero(copia_contador);

                estado_actual = ESTADO_ESPERA;
                break;

            default:
                estado_actual = ESTADO_INICIO;
                break;
        }
    }
    return 0;
}

// Aqui saco los millares, centenas, decenas y unidades.
// Al millar le hago % 10 para no salirme de la tabla (65535 / 1000 = 65)
void dividir_numero(uint16_t valor)
{
    digitos[0] = (valor / 1000) % 10;
    digitos[1] = (valor / 100) % 10;
    digitos[2] = (valor / 10) % 10;
    digitos[3] = valor % 10;
}

// Pongo en los 7 segmentos el patron del numero, si no es de 0 a 9 los dejo todos apagados
void dibujar_numero(uint8_t digito)
{
    uint8_t patron = 0;

    if (digito <= 9) {
        patron = patrones[digito];
    }

    for (uint8_t i = 0; i < 7; i++) {
        escribir_pin(&pines_segmentos[i], (patron >> i) & 0x1, SEGMENTO_ACTIVO_ALTO);
    }
}

// Apago los 4 digitos y los 7 segmentos
void apagar_display(void)
{
    for (uint8_t i = 0; i < 4; i++) {
        escribir_pin(&pines_digitos[i], 0, DIGITO_ACTIVO_ALTO);
    }
    for (uint8_t i = 0; i < 7; i++) {
        escribir_pin(&pines_segmentos[i], 0, SEGMENTO_ACTIVO_ALTO);
    }
}

// Esta la llama TIM2 cada TIEMPO_DIGITO_MS y cada vez prendo un solo digito.
// Como es tan rapido el ojo los ve todos prendidos a la vez
void actualizar_display(void)
{
    // Primero apago todos los digitos para que no se vea el numero anterior (efecto fantasma)
    for (uint8_t i = 0; i < 4; i++) {
        escribir_pin(&pines_digitos[i], 0, DIGITO_ACTIVO_ALTO);
    }

    // Paso al siguiente digito: 0 -> 1 -> 2 -> 3 -> 0
    digito_activo = (digito_activo + 1) & 0x3;

    // Primero armo el numero en los segmentos y despues prendo el digito
    dibujar_numero(digitos[digito_activo]);

    escribir_pin(&pines_digitos[digito_activo], 1, DIGITO_ACTIVO_ALTO);
}

// Blink: lo llama TIM3 cada 250 ms y cambia el LED de PH1.
// Con ^= se invierte solo el bit 1 del ODR: si estaba en 1 queda en 0 y al reves.
void led_estado(void)
{
    GPIOH->ODR ^= GPIO_ODR_OD1; // PH1 ^= 1
}

// Valores de los registros que saque del manual de referencia RM0383:
// MODER   00 entrada, 01 salida, 10 funcion alterna, 11 analogico
// OTYPER  0 push-pull, 1 open-drain
// OSPEEDR 00 baja, 01 media, 10 rapida, 11 muy rapida
// PUPDR   00 sin pull, 01 pull-up, 10 pull-down
void init_GPIO(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;        // se puso en 1 GPIOAEN para darle reloj al puerto A (PA11, PA12)
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;        // se puso en 1 GPIOBEN para darle reloj al puerto B (PB7, PB12)
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;        // se puso en 1 GPIOCEN para darle reloj al puerto C (digitos, PC11, PC13 y fotocompuertas)
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;        // se puso en 1 GPIODEN para darle reloj al puerto D (PD2)
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOHEN;        // se puso en 1 GPIOHEN para darle reloj al puerto H (PH1, led_estado)

    // Antes de volverlos salida los dejo en nivel de apagado, asi el display no parpadea al arrancar
    apagar_display();

    // Segmento A, PB12
    GPIOB->MODER   &= ~GPIO_MODER_MODE12;       // se limpio el registro, se puso en 00
    GPIOB->MODER   |=  GPIO_MODER_MODE12_0;     // aqui se puso un 01 para configurar como salida
    GPIOB->OTYPER  &= ~GPIO_OTYPER_OT12;        // se puso en 0 para que sea push-pull
    GPIOB->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED12;   // se puso en 00, velocidad baja, para un LED alcanza
    GPIOB->PUPDR   &= ~GPIO_PUPDR_PUPD12;       // se puso en 00, sin pull-up ni pull-down porque es salida

    // Segmento B, PA12
    GPIOA->MODER   &= ~GPIO_MODER_MODE12;       // se limpio el registro, se puso en 00
    GPIOA->MODER   |=  GPIO_MODER_MODE12_0;     // aqui se puso un 01 para configurar como salida
    GPIOA->OTYPER  &= ~GPIO_OTYPER_OT12;        // se puso en 0 para que sea push-pull
    GPIOA->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED12;   // se puso en 00, velocidad baja
    GPIOA->PUPDR   &= ~GPIO_PUPDR_PUPD12;       // se puso en 00, sin pull-up ni pull-down

    // Segmento C, PC13
    // Ojo: PC13 tambien es el boton azul de la Nucleo, no lo puedo presionar con el programa corriendo.
    // Ademas este pin solo aguanta unos 3 mA y maximo 2 MHz, por eso lo dejo en velocidad baja
    GPIOC->MODER   &= ~GPIO_MODER_MODE13;       // se limpio el registro, se puso en 00
    GPIOC->MODER   |=  GPIO_MODER_MODE13_0;     // aqui se puso un 01 para configurar como salida
    GPIOC->OTYPER  &= ~GPIO_OTYPER_OT13;        // se puso en 0 para que sea push-pull
    GPIOC->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED13;   // se puso en 00, velocidad baja (PC13 no puede mas de 2 MHz)
    GPIOC->PUPDR   &= ~GPIO_PUPDR_PUPD13;       // se puso en 00, sin pull-up ni pull-down

    // Segmento D, PC11
    GPIOC->MODER   &= ~GPIO_MODER_MODE11;       // se limpio el registro, se puso en 00
    GPIOC->MODER   |=  GPIO_MODER_MODE11_0;     // aqui se puso un 01 para configurar como salida
    GPIOC->OTYPER  &= ~GPIO_OTYPER_OT11;        // se puso en 0 para que sea push-pull
    GPIOC->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED11;   // se puso en 00, velocidad baja
    GPIOC->PUPDR   &= ~GPIO_PUPDR_PUPD11;       // se puso en 00, sin pull-up ni pull-down

    // Segmento E, PD2
    GPIOD->MODER   &= ~GPIO_MODER_MODE2;        // se limpio el registro, se puso en 00
    GPIOD->MODER   |=  GPIO_MODER_MODE2_0;      // aqui se puso un 01 para configurar como salida
    GPIOD->OTYPER  &= ~GPIO_OTYPER_OT2;         // se puso en 0 para que sea push-pull
    GPIOD->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED2;    // se puso en 00, velocidad baja
    GPIOD->PUPDR   &= ~GPIO_PUPDR_PUPD2;        // se puso en 00, sin pull-up ni pull-down

    // Segmento F, PA11
    GPIOA->MODER   &= ~GPIO_MODER_MODE11;       // se limpio el registro, se puso en 00
    GPIOA->MODER   |=  GPIO_MODER_MODE11_0;     // aqui se puso un 01 para configurar como salida
    GPIOA->OTYPER  &= ~GPIO_OTYPER_OT11;        // se puso en 0 para que sea push-pull
    GPIOA->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED11;   // se puso en 00, velocidad baja
    GPIOA->PUPDR   &= ~GPIO_PUPDR_PUPD11;       // se puso en 00, sin pull-up ni pull-down

    // Segmento G, PB7
    GPIOB->MODER   &= ~GPIO_MODER_MODE7;        // se limpio el registro, se puso en 00
    GPIOB->MODER   |=  GPIO_MODER_MODE7_0;      // aqui se puso un 01 para configurar como salida
    GPIOB->OTYPER  &= ~GPIO_OTYPER_OT7;         // se puso en 0 para que sea push-pull
    GPIOB->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED7;    // se puso en 00, velocidad baja
    GPIOB->PUPDR   &= ~GPIO_PUPDR_PUPD7;        // se puso en 00, sin pull-up ni pull-down

    // DIG1, PC10
    GPIOC->MODER   &= ~GPIO_MODER_MODE10;       // se limpio el registro, se puso en 00
    GPIOC->MODER   |=  GPIO_MODER_MODE10_0;     // aqui se puso un 01 para configurar como salida
    GPIOC->OTYPER  &= ~GPIO_OTYPER_OT10;        // se puso en 0 para que sea push-pull
    GPIOC->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED10;   // se puso en 00, velocidad baja
    GPIOC->PUPDR   &= ~GPIO_PUPDR_PUPD10;       // se puso en 00, sin pull-up ni pull-down

    // DIG2, PC6
    GPIOC->MODER   &= ~GPIO_MODER_MODE6;        // se limpio el registro, se puso en 00
    GPIOC->MODER   |=  GPIO_MODER_MODE6_0;      // aqui se puso un 01 para configurar como salida
    GPIOC->OTYPER  &= ~GPIO_OTYPER_OT6;         // se puso en 0 para que sea push-pull
    GPIOC->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED6;    // se puso en 00, velocidad baja
    GPIOC->PUPDR   &= ~GPIO_PUPDR_PUPD6;        // se puso en 00, sin pull-up ni pull-down

    // DIG3, PC5
    GPIOC->MODER   &= ~GPIO_MODER_MODE5;        // se limpio el registro, se puso en 00
    GPIOC->MODER   |=  GPIO_MODER_MODE5_0;      // aqui se puso un 01 para configurar como salida
    GPIOC->OTYPER  &= ~GPIO_OTYPER_OT5;         // se puso en 0 para que sea push-pull
    GPIOC->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED5;    // se puso en 00, velocidad baja
    GPIOC->PUPDR   &= ~GPIO_PUPDR_PUPD5;        // se puso en 00, sin pull-up ni pull-down

    // DIG4, PC12
    GPIOC->MODER   &= ~GPIO_MODER_MODE12;       // se limpio el registro, se puso en 00
    GPIOC->MODER   |=  GPIO_MODER_MODE12_0;     // aqui se puso un 01 para configurar como salida
    GPIOC->OTYPER  &= ~GPIO_OTYPER_OT12;        // se puso en 0 para que sea push-pull
    GPIOC->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED12;   // se puso en 00, velocidad baja
    GPIOC->PUPDR   &= ~GPIO_PUPDR_PUPD12;       // se puso en 00, sin pull-up ni pull-down

    // led_estado (blink), PH1
    GPIOH->ODR     &= ~GPIO_ODR_OD1;            // se puso en 0 el bit 1 del ODR para que el LED arranque apagado
    GPIOH->MODER   &= ~GPIO_MODER_MODE1;        // se limpio el registro, se puso en 00
    GPIOH->MODER   |=  GPIO_MODER_MODE1_0;      // aqui se puso un 01 para configurar como salida
    GPIOH->OTYPER  &= ~GPIO_OTYPER_OT1;         // se puso en 0 para que sea push-pull
    GPIOH->OSPEEDR &= ~GPIO_OSPEEDR_OSPEED1;    // se puso en 00, velocidad baja
    GPIOH->PUPDR   &= ~GPIO_PUPDR_PUPD1;        // se puso en 00, sin pull-up ni pull-down

    // exit1, PC3
    GPIOC->MODER   &= ~GPIO_MODER_MODE3;        // se limpio el registro y se dejo en 00 para que sea entrada
    GPIOC->PUPDR   &= ~GPIO_PUPDR_PUPD3;        // se puso en 00, sin pull porque el circuito ya tiene su resistencia

    // exit2, PC1
    GPIOC->MODER   &= ~GPIO_MODER_MODE1;        // se limpio el registro y se dejo en 00 para que sea entrada
    GPIOC->PUPDR   &= ~GPIO_PUPDR_PUPD1;        // se puso en 00, sin pull porque el circuito ya tiene su resistencia
}

// El SysTick es parte del nucleo Cortex-M4, por eso no sale en el RM0383 sino en el manual de programacion (PM0214).
// El procesador va a 16 MHz, entonces 16000 cuentas son 1 ms
void init_SysTick(void)
{
    SysTick->CTRL = 0;                          // se limpio CTRL para tenerlo apagado mientras lo configuro
    SysTick->LOAD = (16000 - 1);                // se puso 15999 para que llegue a 0 cada 1 ms
    SysTick->VAL  = 0;                          // se puso en 0 el valor actual para que arranque desde LOAD

    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk  // se puso en 1 CLKSOURCE para que use el reloj del procesador (16 MHz)
                  | SysTick_CTRL_TICKINT_Msk    // se puso en 1 TICKINT para que haga interrupcion cuando llega a 0
                  | SysTick_CTRL_ENABLE_Msk;    // se puso en 1 ENABLE para arrancarlo
}

void init_EXTI(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;       // se puso en 1 SYSCFGEN para darle reloj al SYSCFG, que es el que conecta los pines con las EXTI

    // EXTICR[0] maneja las lineas 0 a 3, cada linea usa 4 bits
    SYSCFG->EXTICR[0] &= ~(SYSCFG_EXTICR1_EXTI1 | SYSCFG_EXTICR1_EXTI3);       // se limpiaron los campos de EXTI1 y EXTI3, quedan en 0000
    SYSCFG->EXTICR[0] |=  (SYSCFG_EXTICR1_EXTI1_PC | SYSCFG_EXTICR1_EXTI3_PC); // aqui se puso 0010 en los dos para que vengan del puerto C (PC1 y PC3)

    EXTI->RTSR |=  EXTI_RTSR_TR3;               // se puso en 1 TR3 para que exit1 (PC3) dispare con flanco de subida
    EXTI->FTSR &= ~EXTI_FTSR_TR3;               // se puso en 0 TR3 para que PC3 no dispare con flanco de bajada

    EXTI->RTSR &= ~EXTI_RTSR_TR1;               // se puso en 0 TR1 para que PC1 no dispare con flanco de subida
    EXTI->FTSR |=  EXTI_FTSR_TR1;               // se puso en 1 TR1 para que exit2 (PC1) dispare con flanco de bajada

    EXTI->PR   = (EXTI_PR_PR1 | EXTI_PR_PR3);   // se escribio 1 en PR1 y PR3 para borrar las banderas viejas
    EXTI->IMR |= (EXTI_IMR_MR1 | EXTI_IMR_MR3); // se puso en 1 MR1 y MR3 para habilitar las dos lineas

    // No les cambie la prioridad, todas las interrupciones quedan con la misma (la de fabrica, 0).
    // Asi ninguna interrumpe a otra: si llegan dos al tiempo, la segunda espera a que termine la primera
    // y su bandera queda guardada mientras tanto.
    NVIC_EnableIRQ(EXTI3_IRQn);
    NVIC_EnableIRQ(EXTI1_IRQn);
}

// TIM2 para el display: 16 MHz / 16 = 1 us por cuenta, y 1000 cuentas son 1 ms
void init_TIM2(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;         // se puso en 1 TIM2EN para darle reloj a TIM2 (esta en el bus APB1)

    TIM2->CR1 = 0;                              // se limpio CR1, queda contando hacia arriba y apagado
    TIM2->PSC = (16 - 1);                       // se puso 15 en el prescaler para que cada cuenta sea 1 us
    TIM2->ARR = (1000 * TIEMPO_DIGITO_MS - 1);  // se puso 999 (con 1 ms) para que se desborde cada TIEMPO_DIGITO_MS
    TIM2->CNT = 0;                              // se puso el contador en 0
    TIM2->CR1 |= TIM_CR1_ARPE;                  // se puso en 1 ARPE para que el ARR use precarga

    TIM2->SR &= ~TIM_SR_UIF;                    // se puso en 0 UIF por si habia quedado alguna bandera

    TIM2->DIER |= TIM_DIER_UIE;                 // se puso en 1 UIE para que haga interrupcion en cada update

    NVIC_EnableIRQ(TIM2_IRQn);

    TIM2->CR1 |= TIM_CR1_CEN;                   // se puso en 1 CEN para arrancar el timer
}

// TIM3 lo uso solo para led_estado, no lo comparto con nada mas.
// 16 MHz / 16000 = 1 ms por cuenta, y 250 cuentas son 250 ms
void init_TIM3(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;         // se puso en 1 TIM3EN para darle reloj a TIM3 (esta en el bus APB1)

    TIM3->CR1 = 0;                              // se limpio CR1, queda contando hacia arriba y apagado
    TIM3->PSC = (16000 - 1);                    // se puso 15999 en el prescaler para que cada cuenta sea 1 ms
    TIM3->ARR = (250 - 1);                      // se puso 249 para que se desborde cada 250 ms
    TIM3->CNT = 0;                              // se puso el contador en 0
    TIM3->CR1 |= TIM_CR1_ARPE;                  // se puso en 1 ARPE para que el ARR use precarga


    TIM3->SR &= ~TIM_SR_UIF;                    // se puso en 0 UIF por si habia quedado alguna bandera

    TIM3->DIER |= TIM_DIER_UIE;                 // se puso en 1 UIE para que haga interrupcion en cada update

    NVIC_EnableIRQ(TIM3_IRQn);

    TIM3->CR1 |= TIM_CR1_CEN;                   // se puso en 1 CEN para arrancar el timer
}

// Escribo el pin en el ODR: con |= pongo en 1 solo ese bit y con &= ~ lo pongo en 0,
// los demas bits del puerto quedan como estaban.
// Como todas las interrupciones tienen la misma prioridad, ninguna corta a otra a mitad de la escritura.
// activo_alto me dice si "prendido" es un 1 o un 0 en el pin
static void escribir_pin(const PinGPIO *p, uint8_t encendido, uint8_t activo_alto)
{
    uint8_t nivel = activo_alto ? encendido : !encendido;

    if (nivel) {
        p->puerto->ODR |= p->mascara;           // se puso en 1 el bit ODy del pin con su mascara, pin en alto
    } else {
        p->puerto->ODR &= ~p->mascara;          // se puso en 0 el bit ODy del pin con su mascara, pin en bajo
    }
}

// El SysTick solo cuenta milisegundos, no toca nada mas
void SysTick_Handler(void)
{
    ticks_ms++;                                 // cuando se llena vuelve a 0, pero la resta del antirrebote igual funciona
}

void TIM2_IRQHandler(void)
{
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR &= ~TIM_SR_UIF;                // se puso en 0 UIF para borrar la bandera
        actualizar_display();
    }
}

void TIM3_IRQHandler(void)
{
    if (TIM3->SR & TIM_SR_UIF) {
        TIM3->SR &= ~TIM_SR_UIF;                // se puso en 0 UIF para borrar la bandera
        led_estado();
    }
}

// exit1 (PC3), flanco de subida, suma
void EXTI3_IRQHandler(void)
{
    if (EXTI->PR & EXTI_PR_PR3) {
        EXTI->PR |= EXTI_PR_PR3;                 // se escribio 1 en PR3 para borrar la bandera

        // Solo cuento si ya pasaron ANTIRREBOTE_MS desde la ultima vez que conte este sensor
        uint32_t ahora = ticks_ms;
        if ((ahora - ultimo_sensor1) >= ANTIRREBOTE_MS) {
            ultimo_sensor1 = ahora;
            contador++;                         // si pasa de 65535 vuelve a 0
            bandera_cambio = 1;
        }
    }
}

// exit2 (PC1), flanco de bajada, resta
void EXTI1_IRQHandler(void)
{
    if (EXTI->PR & EXTI_PR_PR1) {
        EXTI->PR |= EXTI_PR_PR1;                 // se escribio 1 en PR1 para borrar la bandera

        // Igual que en exit1, solo cuento si ya paso el tiempo del antirrebote
        uint32_t ahora = ticks_ms;
        if ((ahora - ultimo_sensor2) >= ANTIRREBOTE_MS) {
            ultimo_sensor2 = ahora;
            contador--;                         // si baja de 0 se va a 65535
            bandera_cambio = 1;
        }
    }
}
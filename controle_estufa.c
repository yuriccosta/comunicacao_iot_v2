#include <stdio.h>               // Biblioteca padrão para entrada e saída
#include <string.h>              // Biblioteca manipular strings
#include <stdlib.h>              // funções para realizar várias operações, incluindo alocação de memória dinâmica (malloc)

#include "pico/stdlib.h"         // Biblioteca da Raspberry Pi Pico para funções padrão (GPIO, temporização, etc.)
#include "hardware/adc.h"        // Biblioteca da Raspberry Pi Pico para manipulação do conversor ADC
#include "pico/cyw43_arch.h"     // Biblioteca para arquitetura Wi-Fi da Pico com CYW43  

#include "lwip/pbuf.h"           // Lightweight IP stack - manipulação de buffers de pacotes de rede
#include "lwip/tcp.h"            // Lightweight IP stack - fornece funções e estruturas para trabalhar com o protocolo TCP
#include "lwip/netif.h"          // Lightweight IP stack - fornece funções e estruturas para trabalhar com interfaces de rede (netif)


#include "hardware/i2c.h"
#include "ssd1306.h"
#include "font.h"
#include <hardware/pio.h>           
#include "hardware/clocks.h"        
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "animacao_matriz.pio.h" // Biblioteca PIO para controle de LEDs WS2818B
#include "credenciais_wifi.h" // Altere o arquivo de exemplo dentro do lib para suas credenciais e retire example do nome


// Definição dos pinos dos LEDs
#define LED_PIN CYW43_WL_GPIO_LED_PIN   // GPIO do CI CYW43
#define LED_PIN_GREEN 11
#define LED_PIN_BLUE 12
#define LED_PIN_RED 13
#define LED_COUNT 25            // Número de LEDs na matriz
#define MATRIZ_PIN 7            // Pino GPIO conectado aos LEDs WS2818B
#define BUZZER_A 21
#define JOY_X 27 // Joystick está de lado em relação ao que foi dito no pdf
#define JOY_Y 26
#define max_value_joy 4065.0 // (4081 - 16) que são os valores extremos máximos lidos pelo meu joystick

#define I2C_PORT i2c1
#define I2C_SDA 14
#define I2C_SCL 15
#define endereco 0x3C

// Declaração de variáveis globais
PIO pio;
uint sm;
static volatile uint32_t last_time_temp_normal = 0; // Variável para armazenar o tempo que a temperatura está normal
static volatile uint32_t last_time_umid_normal = 0; // Variável para armazenar o tempo que a umidade está normal
static volatile uint cor = 0; // Variável para armazenar a cor da borda do display



// Variáveis de configuração para os atuadores
int temp_min =  20; // Temperatura mínima
int temp_max =  37; // Temperatura máxima
volatile int16_t temp_atual; // Temperatura atual
uint umid_min =  30; // Umidade mínima
uint umid_max =  70; // Umidade máxima
volatile uint16_t umid_atual; // Umidade atual
volatile uint16_t alarm_time = 10000; // Tempo de ativação do alarme

// Estrutura para armazenar mensagens
typedef struct {
    char nivel_temp[20]; // Nível de temperatura (baixa, normal, alta)
    char nivel_umid[20]; // Nível de umidade (baixa, normal, alta)
    char string_temp_atual[6]; // Temperatura atual
    char string_umid_atual[6]; // Umidade atual
} msg_t;

msg_t msg; // Cria uma variável para armazenar os avisos

uint padrao_led[10][LED_COUNT] = {
    {0, 0, 1, 0, 0,
     0, 1, 1, 1, 0,
     1, 1, 1, 1, 1,
     1, 1, 1, 1, 1,
     0, 1, 1, 1, 0,
    }, // Umidificador Ativo (Desenho de gota)
    {2, 0, 2, 0, 2,
     0, 2, 2, 2, 0,
     2, 2, 2, 2, 2,
     0, 2, 2, 2, 0,
     2, 0, 2, 0, 2,
    }, // Desumidificador ativo (Desenho de Sol)
    {0, 0, 0, 0, 0,
     0, 0, 0, 0, 0,
     0, 0, 0, 0, 0,
     0, 0, 0, 0, 0,
     0, 0, 0, 0, 0,
    } // Desliga os LEDs
};

// Ordem da matriz de LEDS, útil para poder visualizar na matriz do código e escrever na ordem correta do hardware
int ordem[LED_COUNT] = {0, 1, 2, 3, 4, 9, 8, 7, 6, 5, 10, 11, 12, 13, 14, 19, 18, 17, 16, 15, 20, 21, 22, 23, 24};  

// Rotina para definição da intensidade de cores do led
uint32_t matrix_rgb(unsigned r, unsigned g, unsigned b);

// Rotina para desenhar o padrão de LED
void display_desenho(int number);

// Configuração do PWM
void pwm_setup(uint pin);

// Função para iniciar o buzzer
void iniciar_buzzer(uint pin);

// Função para parar o buzzer
void parar_buzzer(uint pin);

// Função de timer repetitivo
bool repeating_timer_callback(struct repeating_timer *timer);

// Função de callback ao aceitar conexões TCP
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);

// Função de callback para processar requisições HTTP
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);

// Tratamento do request do usuário
void user_request(char **request);



// Função principal
int main()
{
    //Inicializa todos os tipos de bibliotecas stdio padrão presentes que estão ligados ao binário.
    stdio_init_all();


    //Inicializa a arquitetura do cyw43
    while (cyw43_arch_init())
    {
        printf("Falha ao inicializar Wi-Fi\n");
        sleep_ms(100);
        return -1;
    }

    // GPIO do CI CYW43 em nível baixo
    cyw43_arch_gpio_put(LED_PIN, 0);

    // Ativa o Wi-Fi no modo Station, de modo a que possam ser feitas ligações a outros pontos de acesso Wi-Fi.
    cyw43_arch_enable_sta_mode();

    // Conectar à rede WiFI - fazer um loop até que esteja conectado
    printf("Conectando ao Wi-Fi...\n");
    while (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 20000))
    {
        printf("Falha ao conectar ao Wi-Fi\n");
        sleep_ms(100);
        return -1;
    }
    printf("Conectado ao Wi-Fi\n");

    // Caso seja a interface de rede padrão - imprimir o IP do dispositivo.
    if (netif_default)
    {
        printf("IP do dispositivo: %s\n", ipaddr_ntoa(&netif_default->ip_addr));
    }

    // Configura o servidor TCP - cria novos PCBs TCP. É o primeiro passo para estabelecer uma conexão TCP.
    struct tcp_pcb *server = tcp_new();
    if (!server)
    {
        printf("Falha ao criar servidor TCP\n");
        return -1;
    }

    //vincula um PCB (Protocol Control Block) TCP a um endereço IP e porta específicos.
    if (tcp_bind(server, IP_ADDR_ANY, 80) != ERR_OK)
    {
        printf("Falha ao associar servidor TCP à porta 80\n");
        return -1;
    }

    // Coloca um PCB (Protocol Control Block) TCP em modo de escuta, permitindo que ele aceite conexões de entrada.
    server = tcp_listen(server);

    // Define uma função de callback para aceitar conexões TCP de entrada. É um passo importante na configuração de servidores TCP.
    tcp_accept(server, tcp_server_accept);
    printf("Servidor ouvindo na porta 80\n");


    // Configuração do PIO
    pio = pio0; 
    uint offset = pio_add_program(pio, &animacao_matriz_program);
    sm = pio_claim_unused_sm(pio, true);
    animacao_matriz_program_init(pio, sm, offset, MATRIZ_PIN);

    // Configura o LED RGB como PWM
    pwm_setup(LED_PIN_RED);
    pwm_setup(LED_PIN_BLUE);
    pwm_setup(LED_PIN_GREEN);

    // Configuração do ADC
    adc_init();
    adc_gpio_init(JOY_X);
    adc_gpio_init(JOY_Y);

    stdio_init_all();

    // I2C Initialisation. Using it at 400Khz.
    i2c_init(I2C_PORT, 400 * 1000);
    ssd1306_t ssd; // Inicializa a estrutura do display
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C); // Set the GPIO pin function to I2C
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C); // Set the GPIO pin function to I2C
    gpio_pull_up(I2C_SDA); // Pull up the data line
    gpio_pull_up(I2C_SCL); // Pull up the clock line
    ssd1306_init(&ssd, WIDTH, HEIGHT, false, endereco, I2C_PORT); // Inicializa o display
    ssd1306_config(&ssd); // Configura o display
    ssd1306_send_data(&ssd); // Envia os dados para o display

    // Limpa o display. O display inicia com todos os pixels apagados.
    ssd1306_fill(&ssd, false);
    ssd1306_send_data(&ssd);



    struct repeating_timer timer;
    timer.user_data = &msg; // Passa a variável de avisos para o timer
    // Chama a função imediatamente antes de iniciar o timer
    repeating_timer_callback(&timer);
    
    // Configura um timer repetitivo para chamar a função de callback a cada 6 segundos
    add_repeating_timer_ms(6000, repeating_timer_callback, &msg, &timer);


    bool mostrar_nivel = false;
    uint32_t current_time_display = 1501; // Inicializa a variável de tempo para mostrar os avisos assim que iniciar o programa
    uint32_t last_time_display = 0; // Variável para armazenar o tempo da última mensagem no display
    while (true){
        if (current_time_display - last_time_display > 1500){
            mostrar_nivel = !mostrar_nivel;
            last_time_display = current_time_display;
        
            if (mostrar_nivel){ 
                // Atualiza o conteúdo do display com animações
                ssd1306_fill(&ssd, true); // Limpa o display
                ssd1306_rect(&ssd, 3, 3, 122, 58, false, true); // Desenha um retângulo
                ssd1306_draw_string(&ssd, "Temperatura", 8, 10); // Desenha uma string
                ssd1306_draw_string(&ssd, msg.nivel_temp, 8, 20); // Desenha uma string
                ssd1306_line(&ssd, 0, 32, 127, 32, true); // Desenha uma linha divisória no meio da tela
                ssd1306_draw_string(&ssd, "Umidade", 8, 40); // Desenha uma string
                ssd1306_draw_string(&ssd, msg.nivel_umid, 8, 50); // Desenha uma strin
                ssd1306_send_data(&ssd); // Atualiza o display
            } else{
                // Atualiza o conteúdo do display com animações
                ssd1306_fill(&ssd, true); // Limpa o display
                ssd1306_rect(&ssd, 3, 3, 122, 58, false, true); // Desenha um retângulo
                ssd1306_draw_string(&ssd, "Temperatura", 8, 10); // Desenha uma string
                ssd1306_draw_string(&ssd, msg.string_temp_atual, 8, 20); // Desenha uma string
                ssd1306_line(&ssd, 0, 32, 127, 32, true); // Desenha uma linha divisória no meio da tela
                ssd1306_draw_string(&ssd, "Umidade", 8, 40); // Desenha uma string
                ssd1306_draw_string(&ssd, msg.string_umid_atual, 8, 50); // Desenha uma string
                ssd1306_send_data(&ssd); // Atualiza o display
            }
        }
        current_time_display = to_ms_since_boot(get_absolute_time());

        cyw43_arch_poll(); // Necessário para manter o Wi-Fi ativo
    }

    //Desligar a arquitetura CYW43.
    cyw43_arch_deinit();
    return 0;
}



// -------------------------------------- Funções ---------------------------------

// Rotina para definição da intensidade de cores do led
uint32_t matrix_rgb(unsigned r, unsigned g, unsigned b){
    return (g << 24) | (r << 16) | (b << 8);
}

// Rotina para desenhar o padrão de LED
void display_desenho(int number){
    uint32_t valor_led;

    for (int i = 0; i < LED_COUNT; i++){
        // Define a cor do LED de acordo com o padrão
        if (padrao_led[number][ordem[24 - i]] == 1){
            valor_led = matrix_rgb(0, 0, 10); // Azul
        } else if (padrao_led[number][ordem[24 - i]] == 2){
            valor_led = matrix_rgb(30, 10, 0); // Amarelo
        } else{
            valor_led = matrix_rgb(0, 0, 0); // Desliga o LED
        }
        // Atualiza o LED
        pio_sm_put_blocking(pio, sm, valor_led);
    }
}

// Configuração do PWM
void pwm_setup(uint pino) {
    gpio_set_function(pino, GPIO_FUNC_PWM);   // Configura o pino como saída PWM
    uint slice = pwm_gpio_to_slice_num(pino); // Obtém o slice correspondente
    
    pwm_set_wrap(slice, max_value_joy);  // Define o valor máximo do PWM

    pwm_set_enabled(slice, true);  // Habilita o slice PWM
}

// Função para iniciar o buzzer
void iniciar_buzzer(uint pin) {
    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(pin); // Obtém o slice correspondente

    pwm_set_clkdiv(slice_num, 125); // Define o divisor de clock
    pwm_set_wrap(slice_num, 1000);  // Define o valor máximo do PWM

    pwm_set_gpio_level(pin, 10); //Para um som mais baixo foi colocado em 10
    pwm_set_enabled(slice_num, true);
}

// Função para parar o buzzer
void parar_buzzer(uint pin) {
    uint slice_num = pwm_gpio_to_slice_num(pin); // Obtém o slice correspondente
    pwm_set_enabled(slice_num, false); // Desabilita o slice PWM
    gpio_put(pin, 0); // Coloca o pino em nível para garantir que o buzzer está desligado
}

bool repeating_timer_callback(struct repeating_timer *timer) {
    msg_t *msg = (msg_t *) timer->user_data; // Obtém a mensagem

    // Leitura dos valores do joystick
    adc_select_input(1);  
    uint16_t vrx_value = adc_read(); // Lê o valor do eixo x (Umidade)
    adc_select_input(0);  
    uint16_t vry_value = adc_read(); // Lê o valor do eixo y (Temperatura)

    umid_atual = ((vrx_value - 16) / max_value_joy) * 100; // Converte o valor do eixo x para a faixa de 0 a 100
    temp_atual = ((vry_value - 16) / max_value_joy) * 95 - 10;  // Converte o valor do eixo y para a faixa de -10 a 85
    
    
    sprintf(msg->string_temp_atual, "%d C", temp_atual); // Formata a string
    sprintf(msg->string_umid_atual, "%u %%", umid_atual); // Formata a string
    uint32_t current_time_normal = to_ms_since_boot(get_absolute_time()); // Obtém o tempo atual em milissegundos
    
    // Verifica se a temperatura está fora do intervalo
    if (temp_atual < temp_min){
        // Desliga o LED
        pwm_set_gpio_level(LED_PIN_RED, 0);
        pwm_set_gpio_level(LED_PIN_BLUE, 0);
        pwm_set_gpio_level(LED_PIN_GREEN, 0);
        strcpy(msg->nivel_temp, "baixa");
    } else if (temp_atual > temp_max){
        // Ativa o LED vermelho no máximo
        pwm_set_gpio_level(LED_PIN_RED, max_value_joy);
        pwm_set_gpio_level(LED_PIN_BLUE, 0);
        pwm_set_gpio_level(LED_PIN_GREEN, 0);
        strcpy(msg->nivel_temp, "alta");
    } else {
        // Ajusta a intensidade do LED verde de acordo com a temperatura
        pwm_set_gpio_level(LED_PIN_RED, 0);
        pwm_set_gpio_level(LED_PIN_BLUE, 0);
        pwm_set_gpio_level(LED_PIN_GREEN, (temp_atual - temp_min) * max_value_joy / (temp_max - temp_min));
        strcpy(msg->nivel_temp, "normal"); 
        last_time_temp_normal = current_time_normal;
    }

    // Verifica se a umidade está fora do intervalo
    if (umid_atual < umid_min){
        // Ativa umidificador
        display_desenho(0); // Desenha o padrão de umidificador
        strcpy(msg->nivel_umid, "baixa");
    } else if (umid_atual > umid_max){
        // Ativa desumidificador
        display_desenho(1); // Desenha o padrão de desumidificador
        strcpy(msg->nivel_umid, "alta");
    } else{
        // Desliga umidificador e desumidificador
        display_desenho(2); // Desliga os LEDs
        strcpy(msg->nivel_umid, "normal");
        last_time_umid_normal = current_time_normal;
    }

    // Verifica se a temperatura ou umidade estão fora do intervalo por mais de 10 segundos
    if (current_time_normal - last_time_temp_normal > alarm_time|| current_time_normal - last_time_umid_normal > alarm_time){
        // Ativa o buzzer
        iniciar_buzzer(BUZZER_A);
    } else{
        parar_buzzer(BUZZER_A);
    }

    return true;
}

// Função de callback ao aceitar conexões TCP
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    tcp_recv(newpcb, tcp_server_recv);
    return ERR_OK;
}

// Tratamento do request do usuário - digite aqui
void user_request(char **request){

    if (strstr(*request, "GET /update?") != NULL) {
        // Extrair parâmetros da URL
        char *params = strstr(*request, "?") + 1; // Pega tudo após o '?'
        char tempMax[10], tempMin[10], umidMax[10], umidMin[10], alarmTime[10];

        // Extrai os parâmetros da URL
        // Exemplo: "tempMax=30&tempMin=20&umidMax=70&umidMin=30&alarmTime"
        // Lembrar que %[^&] significa "ler até o caractere '&'"
        sscanf(params, "tempMax=%[^&]&tempMin=%[^&]&umidMax=%[^&]&umidMin=%[^&]&alarmTime=%s", tempMax, tempMin, umidMax, umidMin, alarmTime);

        // Atualizar as variáveis globais        // Desliga o LED
        temp_max = atoi(tempMax);
        temp_min = atoi(tempMin);
        umid_max = atoi(umidMax);
        umid_min = atoi(umidMin);
        alarm_time = atoi(alarmTime) * 1000;
    }
};

// Função de callback para processar requisições HTTP
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (!p)
    {
        tcp_close(tpcb);
        tcp_recv(tpcb, NULL);
        return ERR_OK;
    }

    // Alocação do request na memória dinámica
    char *request = (char *)malloc(p->len + 1);
    memcpy(request, p->payload, p->len);
    request[p->len] = '\0';

    printf("Request: %s\n", request);

    // Tratamento de request
    user_request(&request);

    // Cria a resposta HTML
    char html[3000];

    // Instruções html do webserver
    snprintf(html, sizeof(html), // Formatar uma string e armazená-la em um buffer de caracteres
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html\r\n"
             "\r\n"
             "<!DOCTYPE html>"
            "<html lang=\"pt-br\">"
            "<head>"
            "<meta charset=\"UTF-8\">"
            "<title>Controle de Estufa</title>"
            "<style>"
            "body{font-family:sans-serif;background:#f4f6fb;color:#222;margin:0;padding:0;}"
            ".container{max-width:400px;margin:30px auto;background:#fff;padding:18px 22px 16px 22px;border-radius:8px;box-shadow:0 2px 8px #0001;}"
            "h1{font-size:1.4em;margin:0 0 18px 0;text-align:center;}"
            "form{margin-bottom:16px;}"
            "label{display:block;margin:10px 0 2px 0;font-size:1em;}"
            "input[type=number]{width:100%%;padding:4px 6px;margin-bottom:8px;border:1px solid #bbb;border-radius:3px;}"
            "button{display:block;width:100%%;padding:7px 0;background:#2a7be4;color:#fff;border:none;border-radius:4px;font-size:1em;cursor:pointer;transition:.2s;}"
            "button:hover{background:#185ca7;}"
            "h2{font-size:1.1em;margin:18px 0 8px 0;}"
            "p{margin:6px 0;}"
            "span{font-weight:bold;}"
            "</style>"
            "</head>"
            "<body>"
            "<div class=\"container\">"
            "<h1>Monitoramento IoT</h1>"
            "<form action=\"/update\" method=\"GET\">"
            "<label for=\"tempMax\">Temperatura Máxima (°C):</label>"
            "<input type=\"number\" id=\"tempMax\" name=\"tempMax\" value=\"%d\">"
            "<label for=\"tempMin\">Temperatura Mínima (°C):</label>"
            "<input type=\"number\" id=\"tempMin\" name=\"tempMin\" value=\"%d\">"
            "<label for=\"umidMax\">Umidade Máxima (%%):</label>"
            "<input type=\"number\" id=\"umidMax\" name=\"umidMax\" value=\"%d\">"
            "<label for=\"umidMin\">Umidade Mínima (%%):</label>"
            "<input type=\"number\" id=\"umidMin\" name=\"umidMin\" value=\"%d\">"
            "<label for=\"alarmTime\">Tempo de Ativação do Alarme (s):</label>"
            "<input type=\"number\" id=\"alarmTime\" name=\"alarmTime\" value=\"%d\">"
            "<button type=\"submit\">Atualizar</button>"
            "</form>"
            "<h2>Leituras Atuais</h2>"
            "<p>Temperatura: <span id=\"currentTemp\">%s</span> (<span id=\"tempStatus\">%s</span>)</p>"
            "<p>Umidade: <span id=\"currentUmid\">%s</span> (<span id=\"umidStatus\">%s</span>)</p>"
            "</div>"
            "</body>"
            "</html>",
             temp_max, temp_min, umid_max, umid_min, alarm_time, msg.string_temp_atual, msg.nivel_temp, msg.string_umid_atual, msg.nivel_umid);

    // Escreve dados para envio (mas não os envia imediatamente).
    tcp_write(tpcb, html, strlen(html), TCP_WRITE_FLAG_COPY);

    // Envia a mensagem
    tcp_output(tpcb);

    //libera memória alocada dinamicamente
    free(request);
    
    //libera um buffer de pacote (pbuf) que foi alocado anteriormente
    pbuf_free(p);

    return ERR_OK;
}


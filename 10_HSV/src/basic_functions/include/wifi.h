// Инициализация обратных вызовов для событий Wi‑Fi и получения IPv4‑адреса.
void wifi_init(void);
// Блокирующее подключение к точке доступа.
int wifi_connect(char *ssid, char *psk);
// Блокирующее ожидание получения IP‑адреса по DHCP и вывод статуса подключения.
void wifi_wait_for_ip_addr(void);
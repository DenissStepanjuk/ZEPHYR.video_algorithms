// Глобальная структура callback системы net_mgmt.
// Используется для регистрации обработчика сетевых событий Zephyr
// (например получения IPv4-адреса по DHCP).
extern struct net_mgmt_event_callback mgmt_cb;
    
// Обработчик событий net_mgmt.
// Вызывается сетевым стеком Zephyr при наступлении событий,
// на которые подписан mgmt_cb (в данном проекте — получение IPv4-адреса).
void handler(struct net_mgmt_event_callback *cb,
            uint64_t mgmt_event,
            struct net_if *iface);
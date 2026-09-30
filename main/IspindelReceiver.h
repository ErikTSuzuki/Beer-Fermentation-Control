#pragma once

namespace ispindel_receiver {

// Inicializar uma vez; start requer Wi-Fi e filas ja inicializados.
bool createQueue();
void start();

} // namespace ispindel_receiver

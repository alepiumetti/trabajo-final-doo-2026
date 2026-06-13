// main.cpp
#include "nuevo_test.hpp"

// Función auxiliar para mostrar un usuario
// void mostrarUsuario(const Usuario& u) {
//     std::cout << std::left << std::setw(5) << u.id
//               << std::setw(20) << u.nombre
//               << std::setw(25) << u.email
//               << std::setw(20) << u.fechaRegistro << "\n";
// }

int main()
{
    // Cadena de conexión: cambiar según tu configuración
    std::string conexion = GestorUsuarios::buildConnString("config.db");
    // std::string conexion = "host=localhost port=1521 service=XE user=tu_usuario password=tu_password";

    try
    {
        GestorUsuarios gestor(conexion);

        // 1. Insertar algunos usuarios
        Usuario u1 = {1, "Ana Pérez", "ana@example.com", "2025-01-15 10:30:00"};
        gestor.insertarUsuario(u1);

        Usuario u2 = {2, "Luis Gómez", "luis@example.com", "2025-02-20 14:45:00"};
        gestor.insertarUsuario(u2);

        // 2. Consultar por ID
        auto usuario = gestor.obtenerUsuarioPorId(1);
        if (usuario)
        {
            std::cout << "\n--- Usuario encontrado ---\n";
            // mostrarUsuario(*usuario);
            std::cout << *usuario;
        }

        // 3. Listar todos los usuarios
        std::cout << "\n--- Lista de usuarios ---\n";
        std::cout << std::left << std::setw(5) << "ID"
                  << std::setw(20) << "Nombre"
                  << std::setw(25) << "Email"
                  << std::setw(20) << "Fecha registro\n";
        auto todos = gestor.obtenerTodosUsuarios();
        for (const auto &u : todos)
        {
            // mostrarUsuario(u);
            std::cout << u << std::endl;
        }

        // 4. Actualizar email
        gestor.actualizarEmail(2, "nuevo_luis@example.com");

        // 5. Eliminar un usuario (ejemplo)
        gestor.eliminarUsuario(1);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

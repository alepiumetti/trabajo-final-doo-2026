#include <soci/soci.h>
#include <soci/oracle/soci-oracle.h>
#include <string>
#include <vector>
#include <optional>
#include <iostream>
#include <iomanip>
#include <map>
#include <fstream>

struct Usuario {
    int id;
    std::string nombre;
    std::string email;
    std::string fechaRegistro; // Seguimos con string por simplicidad
};

std::ostream& operator<< (std::ostream& os, const struct Usuario& p){
    os << "ID: " << p.id <<" " << p.nombre << " - " << p.email << " - Fecha de Registro: " << p.fechaRegistro;
    return os;
}

// --- ¡Aquí ocurre la magia! ---
namespace soci {

template<>
struct type_conversion<Usuario> {
    // Usamos 'values' como tipo base. Es un contenedor tipo mapa que SOCI entiende.
    typedef values base_type;

    // Convierte de 'values' (DB) a 'Usuario' (C++)
    static void from_base(values const & v, indicator ind, Usuario & u) {
        // El orden de extracción no importa, se hace por nombre de columna.
        u.id = v.get<int>("ID");
        u.nombre = v.get<std::string>("NOMBRE");
        u.email = v.get<std::string>("EMAIL");
        // Asumiendo que la fecha viene como string de la DB
        u.fechaRegistro = v.get<std::string>("FECHA_REGISTRO","");
    }

    // Convierte de 'Usuario' (C++) a 'values' (DB)
    static void to_base(const Usuario & u, values & v, indicator & ind) {
        // Se asignan los valores al objeto 'values' usando el nombre de la columna.
        v.set("ID", u.id);
        v.set("NOMBRE", u.nombre);
        v.set("EMAIL", u.email);
        v.set("FECHA_REGISTRO", u.fechaRegistro);
        ind = i_ok; // Indicamos que el dato no es nulo
    }
};

} // namespace soci

// Clase para manejar operaciones de usuarios
class GestorUsuarios {
private:
    std::unique_ptr<soci::session> sql;

public:
    // Constructor: recibe la cadena de conexión de Oracle
    // Ejemplo: "host=localhost port=1521 service=XE user=scott password=tiger"
    GestorUsuarios(const std::string& connString) {
        try {
            sql = std::make_unique<soci::session>(soci::oracle, connString);
            std::cout << "Conexión exitosa a Oracle.\n";
        } catch (const soci::soci_error& e) {
            throw std::runtime_error("Error de conexión: " + std::string(e.what()));
        }
    }

    // Lector de configuración adaptado para Oracle
    static std::string buildConnString(const std::string& filename) {
        std::ifstream file(filename);
        std::map<std::string, std::string> c;
        std::string line;
        while (std::getline(file, line)) {
            size_t sep = line.find('=');
            if (sep != std::string::npos) c[line.substr(0, sep)] = line.substr(sep + 1);
        }
        
        // Formato SOCI para Oracle: "service=miservicio user=miusuario password=miclave"
        // 'service' puede ser un alias TNS o un EZConnect string como "//localhost:1521/XEPDB1"
        return "service=" + c["DB_SERVICE"] + 
               " user=" + c["DB_USER"] + 
               " password=" + c["DB_PASS"];
    }

    // Insertar un nuevo usuario
    bool insertarUsuario(const Usuario& user) {
        // SOCI ahora sabe cómo convertir 'nuevoUsuario' a algo que Oracle entiende
        try {
            sql->begin();
            *sql << "INSERT INTO persona (ID, NOMBRE, EMAIL, FECHA_REGISTRO) "
                           "VALUES (:ID, :NOMBRE, :EMAIL, TO_DATE(:FECHA_REGISTRO, 'YYYY-MM-DD HH24:MI:SS'))",
                soci::use(user);
            sql->commit();
            std::cout << "Usuario insertado automágicamente.\n";
            return true;
        } catch (const soci::soci_error& e) {
            sql->rollback();
            std::cerr << "Error insertando: " << e.what() << "\n";
            return false;
        }
    }

    // Consultar usuario por ID (devuelve std::optional)
    std::optional<Usuario> obtenerUsuarioPorId(int id) {
        std::cout<<"Ingreso a obtener usuario\n";
        Usuario u;
        //bool encontrado = false;

        try {
            *sql << "SELECT ID, NOMBRE, EMAIL, TO_CHAR(FECHA_REGISTRO, 'YYYY-MM-DD HH24:MI:SS') as FECHA_REGISTRO "
                           "FROM persona WHERE ID = :id",
                            soci::into(u.id), soci::into(u.nombre),
                            soci::into(u.email), soci::into(u.fechaRegistro),
                            soci::use(id);
               /* soci::into(u), // SOCI vuelca los datos directamente en la estructura
                soci::use(id);*/
            //encontrado = true;
            return u;
        } catch (const soci::soci_error& e) {
            if (e.get_error_category() == soci::soci_error::no_data) {
                std::cerr << "Usuario no encontrado.\n";
                return std::nullopt;
            } else {
                std::cerr << "Error en consulta: " << e.what() << "\n";
                return std::nullopt;
            }
        }

       /* if (encontrado) {
            std::cout << "Usuario encontrado: " << usuarioConsultado.nombre << "\n";
        }*/
    }

    // Consultar todos los usuarios
    std::vector<Usuario> obtenerTodosUsuarios() {
        std::vector<Usuario> usuarios;
        try {
           /* soci::rowset<soci::row> rs = (sql->prepare <<
                "SELECT id, nombre, email, TO_CHAR(FECHA_REGISTRO, 'YYYY-MM-DD HH24:MI:SS') as FECHA_REGISTRO "
                "FROM persona ORDER BY id");
            for (const soci::row& row : rs) {
                Usuario u;
                u.id = row.get<int>(0);
                u.nombre = row.get<std::string>(1);
                u.email = row.get<std::string>(2);
                u.fechaRegistro = row.get<std::string>(3);
                usuarios.push_back(u);
            }*/
            std::string nombre, email, fecha;
            int id;
            soci::statement st = (sql->prepare << "SELECT id, nombre, email, TO_CHAR(FECHA_REGISTRO, 'YYYY-MM-DD HH24:MI:SS') as FECHA_REGISTRO FROM persona ORDER BY id", soci::into(id), soci::into(nombre), soci::into(email), soci::into(fecha));
            
            st.execute();
            while (st.fetch()) {
                Usuario u = {id, nombre, email, fecha};
                usuarios.push_back(u);
            }
        } catch (const soci::soci_error& e) {
            std::cerr << "Error al listar: " << e.what() << std::endl;
        }
        return usuarios;
        
    }

    // Actualizar email de un usuario
   bool actualizarEmail(int id, const std::string& nuevoEmail) {
    try {
        // 1. Verificar si existe el registro
        int count = 0;
        *sql << "SELECT COUNT(*) FROM persona WHERE id = :id", soci::use(id), soci::into(count);
        if (count == 0) {
            std::cerr << "ID " << id << " no encontrado.\n";
            return false;
        }

        // 2. Realizar la actualización
        *sql << "UPDATE persona SET email = :email WHERE id = :id",
            soci::use(nuevoEmail), soci::use(id);

        std::cout << "Correo actualizado correctamente.\n";
        return true;
    } catch (const soci::soci_error& e) {
        std::cerr << "Error en la actualización: " << e.what() << std::endl;
        return false;
    }
}

    // Eliminar usuario (opcional)
    bool eliminarUsuario(int id) {
        try {
            int count = 0;
            *sql << "SELECT COUNT(*) FROM persona WHERE id = :id", soci::use(id), soci::into(count);
            if (count == 0) {
                std::cerr << "ID " << id << " no encontrado.\n";
                return false;
            }
            *sql << "DELETE FROM persona WHERE id = :id", soci::use(id);
            std::cout << "Usuario eliminado.\n";
            return true;            
        } catch (const soci::soci_error& e) {
            std::cerr << "Error al eliminar: " << e.what() << "\n";
            return false;
        }
    }
};
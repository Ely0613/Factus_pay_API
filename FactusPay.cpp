#include <iostream>
#include <string>
#include <mutex>
#include <cmath>
#include <ctime>
#include <cctype>
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

// Fecha UTC en formato ISO 8601 (thread-safe)
std::string ahoraISO() {
    std::time_t t = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &utc);
    return std::string(buf) + ".000000Z";
}

// Responde con un error en formato uniforme
void responderError(httplib::Response& res, int code, const std::string& msg) {
    res.status = code;
    res.set_content(json{{"message", msg}}.dump(), "application/json");
}

// Valida que el string sea un número como "10000" o "10000.50"
// (solo dígitos y un punto opcional, con al menos un dígito antes y uno después)
// Devuelve: 0 = ok, 1 = no numérico, 2 = más de 2 decimales
int validarFormatoMonto(const std::string& s) {
    size_t punto = s.find('.');
    std::string entera = (punto == std::string::npos) ? s : s.substr(0, punto);
    std::string decimal = (punto == std::string::npos) ? "" : s.substr(punto + 1);

    if (entera.empty()) return 1;
    if (punto != std::string::npos && decimal.empty()) return 1;

    for (char c : entera)
        if (!std::isdigit(static_cast<unsigned char>(c))) return 1;
    for (char c : decimal)
        if (!std::isdigit(static_cast<unsigned char>(c))) return 1;

    if (decimal.size() > 2) return 2;
    return 0;
}

int main() {
    httplib::Server svr;
    const std::string token = "1|abcdEfGhIjKlMnOpQrStUvWxYz1234567890";
    const std::string emailValido = "sandboxv2@factus.com.co";
    const std::string passwordValido = "sandbox2026%";

    json collec = json::array();
    std::mutex mtx;

    // ---------------- POST /auth ----------------
    svr.Post("/auth", [&](const httplib::Request& req, httplib::Response& res) {
        json identificacion = json::parse(req.body, nullptr, false);

        if (identificacion.is_discarded() || !identificacion.is_object()) {
            responderError(res, 400, "JSON invalido");
            return;
        }

        if (!identificacion.contains("email") || !identificacion["email"].is_string() ||
            identificacion["email"].get<std::string>().empty()) {
            responderError(res, 422, "Falta el campo email");
            return;
        }

        if (!identificacion.contains("password") || !identificacion["password"].is_string() ||
            identificacion["password"].get<std::string>().empty()) {
            responderError(res, 422, "Falta el campo password");
            return;
        }

        if (identificacion["email"].get<std::string>() == emailValido &&
            identificacion["password"].get<std::string>() == passwordValido) {
            json respuesta = {{"token", token}};
            res.set_content(respuesta.dump(), "application/json");
            return;
        }

        responderError(res, 401, "Credenciales invalidas");
    });

    // ---------------- POST /v1/collections ----------------
    svr.Post("/v1/collections", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_header_value("Authorization") != "Bearer " + token) {
            responderError(res, 401, "No autorizado");
            return;
        }

        json data = json::parse(req.body, nullptr, false);
        if (data.is_discarded() || !data.is_object()) {
            responderError(res, 400, "JSON invalido");
            return;
        }

        if (!data.contains("reference_code") || !data["reference_code"].is_string() ||
            data["reference_code"].get<std::string>().empty()) {
            responderError(res, 422, "Falta el campo reference_code");
            return;
        }

        std::string ref = data["reference_code"].get<std::string>();
        if (ref.size() > 100) {
            responderError(res, 422, "reference_code tiene un maximo de 100 caracteres");
            return;
        }

        if (!data.contains("amount")) {
            responderError(res, 422, "Falta el campo amount");
            return;
        }

        if (!data["amount"].is_number()) {
            responderError(res, 422, "El campo amount debe ser numerico");
            return;
        }

        double monto = data["amount"].get<double>();

        if (monto < 10000 || monto > 12000000) {
            responderError(res, 422, "El campo amount debe estar entre 10000 y 12000000.");
            return;
        }

        if (std::fabs(monto * 100 - std::round(monto * 100)) > 1e-6) {
            responderError(res, 422, "El campo amount admite maximo 2 decimales.");
            return;
        }

        std::lock_guard<std::mutex> lock(mtx);

        for (auto& c : collec) {
            if (c["reference_code"] == ref) {
                c["status"] = "ready";
                json r = {{"data", c}, {"status", "success"}, {"message", "Recaudo ya existente"}};
                res.status = 200;
                res.set_content(r.dump(), "application/json");
                return;
            }
        }

        json nuevo = {
            {"reference_code", ref},
            {"amount", data["amount"]},
            {"status", "started"},
            {"created_at", ahoraISO()},
            {"qr", nullptr}
        };
        collec.push_back(nuevo);

        json r = {{"data", nuevo}, {"status", "success"}, {"message", "Recaudo creado correctamente"}};
        res.status = 201;
        res.set_content(r.dump(), "application/json");
    });

    // ---------------- GET /v1/collections ----------------
    svr.Get("/v1/collections", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_header_value("Authorization") != "Bearer " + token) {
            responderError(res, 401, "No autorizado");
            return;
        }

        bool filtrarStatus = req.has_param("status");
        bool filtrarRef = req.has_param("reference_code");
        std::string status, ref;

        if (filtrarStatus) {
            status = req.get_param_value("status");
            if (status != "started" && status != "ready" && status != "paid" &&
                status != "failed" && status != "rejected") {
                responderError(res, 422,
                    "El campo status debe ser uno de: started, ready, paid, failed, rejected.");
                return;
            }
        }

        if (filtrarRef) {
            ref = req.get_param_value("reference_code");
            if (ref.size() > 100) {
                responderError(res, 422, "reference_code tiene un maximo de 100 caracteres");
                return;
            }
        }

        json resultado = json::array();
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (const auto& c : collec) {
                if (filtrarStatus && c.value("status", "") != status) continue;
                if (filtrarRef && c.value("reference_code", "") != ref) continue;
                resultado.push_back(c);
            }
        }

        json r = {{"data", resultado}, {"status", "success"}};
        res.set_content(r.dump(), "application/json");
    });

    // ----------------v1/collections/:referenceCode ----------------
    svr.Get(R"(/v1/collections/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_header_value("Authorization") != "Bearer " + token) {
            responderError(res, 401, "No autorizado");
            return;
        }

        std::string ref = req.matches[1];

        std::lock_guard<std::mutex> lock(mtx);

        for (const auto& c : collec) {
            if (c.value("reference_code", "") == ref) {
                json r = {
                    {"data", c},
                    {"status", "success"},
                    {"message", "Recaudo encontrado"}
                };
                res.status = 200;
                res.set_content(r.dump(), "application/json");
                return;
            }
        }

        json r = {
            {"status", "error"},
            {"message", "Recaudo no encontrado"},
            {"data", nullptr}
        };
        res.status = 404;
        res.set_content(r.dump(), "application/json");
    });
    
    std::cout << "Servidor corriendo en http://localhost:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);
}
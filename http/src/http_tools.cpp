#include "../include/http_tools.hpp"
#include "../include/utils.hpp"
#include "../../log/include/log.hpp"
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <string>

LogLevel http_log_level = LogLevel::DEBUG;

/**
 * @brief Send an HTTP response with the given status and body.
 * 
 * @param req The original HTTP request.
 * @param status The HTTP status code.
 * @param body The response body content.
 * @param content_type The content type of the response.
 * @param logger A shared pointer to the logger used for logging.
 * @return The HTTP response object.
 */
template <class Body, class Allocator>
http::message_generator send_(
    http::request<Body, http::basic_fields<Allocator>> const& req,
    http::status status,
    const std::string& body,
    const std::string& content_type = "application/json")
{
    auto logger = LoggerManager::getLogger("http_tools_logger", http_log_level);
    logger->log(LogLevel::DEBUG, "Preparing response with status: " + std::to_string(static_cast<int>(status)));

    http::response<http::string_body> res{status, req.version()};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, content_type);
    res.keep_alive(req.keep_alive());
    res.body() = body;
    res.prepare_payload();

    logger->log(LogLevel::DEBUG, "Response prepared with body: " + body);

    // Return the response as a message generator
    return http::message_generator(std::move(res));
}



/**
 * @brief Handle an HTTP GET request to serve JSON data from a file.
 * 
 * @param req The GET request object.
 * @param app A shared pointer to the Application.
 * @return The HTTP response as a message generator.
 */
template <class Body, class Allocator>
http::message_generator handle_json_data_request(
    http::request<Body, http::basic_fields<Allocator>>&& req,
    std::shared_ptr<Application> app)
{
    auto logger = LoggerManager::getLogger("http_tools_logger", http_log_level);
    logger->log(LogLevel::DEBUG, "Received GET request for JSON data.");

    try {
        // Define the path to the JSON file
        const std::string json_file_path = "www/data/mock.json";

        // Open the JSON file
        std::ifstream json_file(json_file_path);
        if (!json_file.is_open()) {
            logger->log(LogLevel::ERROR, "Failed to open JSON file: " + json_file_path);
            return send_(req, http::status::internal_server_error, R"({"error": "Failed to open JSON file."})");
        }

        // Parse the JSON file
        nlohmann::json json_data;
        json_file >> json_data;
        json_file.close();  // Close the file

        // Convert JSON to string
        std::string response_body = json_data.dump();

        // Send the JSON data as the response
        return send_(req, http::status::ok, response_body, "application/json");
    } catch (const std::exception& e) {
        logger->log(LogLevel::ERROR, "Exception caught while serving JSON data: " + std::string(e.what()));
        return send_(req, http::status::internal_server_error, R"({"error": ")" + std::string(e.what()) + "\"}");
    }
}


/**
 * @brief Handle an HTTP POST request.
 * 
 * @param req The POST request object.
 * @param logger A shared pointer to the logger used for logging.
 * @return The HTTP response as a message generator.
 *
 * @problem response times out if message is too big
 */
template <class Body, class Allocator>
http::message_generator handle_post_request(
    http::request<Body, http::basic_fields<Allocator>>&& req,
    std::shared_ptr<Application> app)
{
    auto logger = LoggerManager::getLogger("http_tools_logger", http_log_level);
    try {
        auto json_obj = nlohmann::json::parse(req.body());

        // Check if the JSON contains both 'message' and 'context'
        if (json_obj.contains("message")) {
            std::string message = json_obj["message"].template get<std::string>();
            logger->log(LogLevel::DEBUG, "Received LLM message: " + message);

            // Handle context if provided
            ollama::response context;
            if (json_obj.contains("context")) {
                context = ollama::response(json_obj["context"].dump());
                logger->log(LogLevel::DEBUG, "Received context for LLM.");
            }

            // Add the query with context to the queue and get the query ID
            std::string query_id = app->add_query(message, context);

            nlohmann::json response_json;
            response_json["query_id"] = query_id;
            response_json["status"] = "Query added to the queue";

            return send_(req, http::status::ok, response_json.dump(), "application/json");
        } else {
            logger->log(LogLevel::ERROR, R"({"error": "Missing 'message' field in JSON request."})");
            return send_(req, http::status::bad_request, R"({"error": "Missing 'message' field in JSON request."})");
        }
    } catch (const nlohmann::json::exception& e) {
        logger->log(LogLevel::ERROR, "JSON parsing exception: " + std::string(e.what()));
        return send_(req, http::status::bad_request, R"({"error": "Invalid JSON format."})");
    } catch (const std::exception& e) {
        logger->log(LogLevel::ERROR, "Exception caught: " + std::string(e.what()));
        return send_(req, http::status::internal_server_error, R"({"error": ")" + std::string(e.what()) + "\"}");
    }
}



/**
 * @brief Handle an HTTP GET request and serve the requested file.
 * 
 * @param doc_root The document root directory.
 * @param req The GET request object.
 * @param logger A shared pointer to the logger used for logging.
 * @return The HTTP response as a message generator.
 */
template <class Body, class Allocator>
http::message_generator handle_get_request(
    beast::string_view doc_root,
    http::request<Body, http::basic_fields<Allocator>>&& req,
    std::shared_ptr<Application> app)  // Added the Application shared pointer
{
    auto logger = LoggerManager::getLogger("http_tools_logger", http_log_level);
    logger->log(LogLevel::DEBUG, "Received GET request for target: " + std::string(req.target()));

    try {
        // Extract the target path
        std::string target = std::string(req.target());

        // Check if the request is for querying the status of a query
        if (target.rfind("/query_status/", 0) == 0) {
            // Extract the query ID from the URL (e.g., /query_status/{query_id})
            std::string query_id = target.substr(14);  // 14 is the length of "/query_status/"
            logger->log(LogLevel::DEBUG, "Query status request for query_id: " + query_id);

            // Get the status from the Application
            std::string status = app->get_query_status(query_id);

            // Create a JSON response with the query status
            nlohmann::json response_json;
            response_json["query_id"] = query_id;
            response_json["status"] = status;

            // Send the JSON response back to the client
            return send_(req, http::status::ok, response_json.dump(), "application/json");
        }

        // If not a query status request, proceed with serving a file
        std::string path = path_cat(doc_root, target);
        logger->log(LogLevel::DEBUG, "Computed path: " + path);

        if (target.back() == '/') {
            path.append("index.html");
            logger->log(LogLevel::DEBUG, "Appended index.html to path: " + path);
        }

        beast::error_code ec;
        http::file_body::value_type body;
        body.open(path.c_str(), beast::file_mode::scan, ec);

        if (ec == beast::errc::no_such_file_or_directory) {
            logger->log(LogLevel::DEBUG, "File not found: " + path);
            return send_(req, http::status::not_found, "The resource was not found.");
        }

        if (ec) {
            logger->log(LogLevel::ERROR, "Error opening file: " + ec.message());
            return send_(req, http::status::internal_server_error, "Error: " + ec.message());
        }

        auto const size = body.size();
        logger->log(LogLevel::DEBUG, "File opened successfully, size: " + std::to_string(size));

        if (req.method() == http::verb::head) {
            logger->log(LogLevel::DEBUG, "HEAD request, preparing response headers.");
            http::response<http::empty_body> res{http::status::ok, req.version()};
            res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res.set(http::field::content_type, mime_type(path));
            res.content_length(size);
            res.keep_alive(req.keep_alive());
            return res;
        }

        logger->log(LogLevel::DEBUG, "GET request, preparing full response.");
        http::response<http::file_body> res{
            std::piecewise_construct,
            std::make_tuple(std::move(body)),
            std::make_tuple(http::status::ok, req.version())
        };
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, mime_type(path));
        res.content_length(size);
        res.keep_alive(req.keep_alive());
        return res;
    } catch (const std::exception& e) {
        logger->log(LogLevel::ERROR, "Exception caught: " + std::string(e.what()));
        return send_(req, http::status::internal_server_error, R"({"error": ")" + std::string(e.what()) + "\"}");
    }
}

template <class Body, class Allocator>
http::message_generator handle_performance_statistics_request(
    http::request<Body, http::basic_fields<Allocator>>&& req,
    std::shared_ptr<Application> app)
{
    auto logger = LoggerManager::getLogger("http_tools_logger", http_log_level);
    logger->log(LogLevel::DEBUG, "Received request for performance statistics.");

    try {
        // Retrieve the performance statistics as JSON
        nlohmann::json stats_json = app->get_performance_statistics_json();

        // Send the JSON data as the response
        return send_(req, http::status::ok, stats_json.dump(), "application/json");
    } catch (const std::exception& e) {
        logger->log(LogLevel::ERROR, "Exception caught while serving performance statistics: " + std::string(e.what()));
        return send_(req, http::status::internal_server_error, R"({"error": ")" + std::string(e.what()) + "\"}");
    }
}

/**
 * @brief Handle an HTTP request and generate an appropriate response.
 * 
 * This function routes the request to the appropriate handler based on the HTTP method.
 * 
 * @param doc_root The document root directory.
 * @param req The HTTP request object.
 * @param logger A shared pointer to the logger used for logging.
 * @return The HTTP response as a message generator.
 */
template <class Body, class Allocator>
http::message_generator handle_request(
    beast::string_view doc_root,
    boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>>&& req,
    std::shared_ptr<Application> app) { 
    auto logger = LoggerManager::getLogger("http_tools_logger", http_log_level);
    logger->log(LogLevel::DEBUG, "Received request: " + std::string(req.method_string()) + " " + std::string(req.target()));

    auto process_start_time = std::chrono::high_resolution_clock::now();

    http::message_generator response = [&] {
        if (req.method() == http::verb::post && req.target() == "/") {
            logger->log(LogLevel::DEBUG, "Delegating to handle_post_request.");
            return handle_post_request(std::move(req), app);
        } else if (req.method() == http::verb::get && req.target() == "/json_data") {
            logger->log(LogLevel::DEBUG, "Delegating to handle_json_data_request.");
            return handle_json_data_request(std::move(req), app);
        } else if (req.method() == http::verb::get && req.target() == "/performance_statistics") {
            logger->log(LogLevel::DEBUG, "Delegating to handle_performance_statistics_request.");
            return handle_performance_statistics_request(std::move(req), app);
        } else if (req.method() == http::verb::get || req.method() == http::verb::head) {
            logger->log(LogLevel::DEBUG, "Delegating to handle_get_request.");
            return handle_get_request(doc_root, std::move(req), app);
        } else {
            logger->log(LogLevel::DEBUG, "Unknown HTTP method, responding with bad request.");
            return send_(req, http::status::bad_request, "Unknown HTTP-method");
        }
    }();

    auto process_end_time = std::chrono::high_resolution_clock::now();
    auto process_duration = std::chrono::duration_cast<std::chrono::microseconds>(process_end_time - process_start_time).count();
    logger->log(LogLevel::DEBUG, "Time to process request: " + std::to_string(process_duration) + " µs");
    // Log the request processing time
    app->log_performance_metric("Request Processing Duration (µs)", process_duration);

    return response;
}


/**
 * @brief Determine the MIME type based on the file extension.
 * 
 * @param path The file path.
 * @return The MIME type as a string_view.
 */
beast::string_view mime_type(beast::string_view path)
{
    using beast::iequals;
    auto const ext = [&path]
    {
        auto const pos = path.rfind(".");
        if(pos == beast::string_view::npos)
            return beast::string_view{};
        return path.substr(pos);
    }();
    if(iequals(ext, ".htm"))  return "text/html";
    if(iequals(ext, ".html")) return "text/html";
    if(iequals(ext, ".php"))  return "text/html";
    if(iequals(ext, ".css"))  return "text/css";
    if(iequals(ext, ".txt"))  return "text/plain";
    if(iequals(ext, ".js"))   return "application/javascript";
    if(iequals(ext, ".json")) return "application/json";
    if(iequals(ext, ".xml"))  return "application/xml";
    if(iequals(ext, ".swf"))  return "application/x-shockwave-flash";
    if(iequals(ext, ".flv"))  return "video/x-flv";
    if(iequals(ext, ".png"))  return "image/png";
    if(iequals(ext, ".jpe"))  return "image/jpeg";
    if(iequals(ext, ".jpeg")) return "image/jpeg";
    if(iequals(ext, ".jpg"))  return "image/jpeg";
    if(iequals(ext, ".gif"))  return "image/gif";
    if(iequals(ext, ".bmp"))  return "image/bmp";
    if(iequals(ext, ".ico"))  return "image/vnd.microsoft.icon";
    if(iequals(ext, ".tiff")) return "image/tiff";
    if(iequals(ext, ".tif"))  return "image/tiff";
    if(iequals(ext, ".svg"))  return "image/svg+xml";
    if(iequals(ext, ".svgz")) return "image/svg+xml";
    return "application/text";
}

/**
 * @brief Concatenate a base path with a relative path.
 * 
 * @param base The base path.
 * @param path The relative path.
 * @return The concatenated path as a string.
 */
std::string path_cat(beast::string_view base, beast::string_view path)
{
    if(base.empty())
        return std::string(path);
    std::string result(base);
#ifdef BOOST_MSVC
    char constexpr path_separator = '\\';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
    for(auto& c : result)
        if(c == '/')
            c = path_separator;
#else
    char constexpr path_separator = '/';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
#endif
    return result;
}

// Explicit template instantiation for string body requests
template http::message_generator handle_request<http::string_body, std::allocator<char>>(
    beast::string_view doc_root,
    http::request<http::string_body, http::basic_fields<std::allocator<char>>>&& req,
    std::shared_ptr<Application> app);



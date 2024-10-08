#ifndef APPLICATION_HPP
#define APPLICATION_HPP

#include <queue>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <condition_variable>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <sqlite3.h>  // Include SQLite header
#include <SQLiteCpp/SQLiteCpp.h>

#include "../../ollama/include/ollama.hpp"
#include "../../http/include/client.hpp"
#include "../../log/include/log.hpp"

struct MetricStatistic {
    std::string metric_name;
    double average_value;
    double min_value;
    double max_value;
    double total_value;
    int count;
};

/**
 * @brief A structure representing a query to the LLM.
 * 
 * This structure holds all the information related to a single query, 
 * including the prompt, responses, and its state (e.g., running, completed, canceled).
 */
struct Query {
    std::string id;  ///< Unique identifier for the query.
    std::string prompt;  ///< The prompt to be sent to the LLM.
    std::string response;  ///< The full response from the LLM.
    std::vector<std::string> partial_responses;  ///< Accumulated partial responses from the LLM.
    std::atomic<bool> completed{false};  ///< Indicates whether the query has been completed.
    std::atomic<bool> running{false};  ///< Indicates whether the query is currently running.
    std::atomic<bool> canceled{false};  ///< Indicates whether the query has been canceled.
    ollama::response last_context;
};

/**
 * @brief The Application class encapsulates the main logic of the application.
 * 
 * This class is responsible for managing interactions with the Large Language Model (LLM) 
 * through the Ollama API. It handles submitting prompts, receiving responses, and managing 
 * the state of each query. It also provides methods to check the status of ongoing queries 
 * and to cancel them if necessary.
 */
class Application {
public:
    /**
     * @brief Constructs an Application object.
     * 
     * @param ioc The Boost.Asio I/O context that the application will use for asynchronous operations.
     */
    Application(boost::asio::io_context& ioc, ssl::context& ssl_ctx);

    /**
     * @brief Destructor for Application class to close SQLite connection.
     */
    ~Application();

    /**
     * @brief Adds a new query to the application.
     * 
     * Generates a unique query ID, stores the prompt, and places the query in the queue for processing.
     * 
     * @param prompt The prompt to be sent to the LLM.
     * @return The unique ID of the newly added query.
     */
    std::string add_query(const std::string& prompt, const ollama::response& context = ollama::response());

    /**
     * @brief Retrieves the status of a specific query.
     * 
     * The status includes whether the query is running, completed, canceled, and any partial responses received so far.
     * 
     * @param query_id The unique ID of the query.
     * @return A JSON string containing the status of the query.
     */
    std::string get_query_status(const std::string& query_id);

    /**
     * @brief Cancels a specific query.
     * 
     * If the query is currently in progress, it will be marked as canceled and will stop processing.
     * 
     * @param query_id The unique ID of the query to cancel.
     */
    void cancel_query(const std::string& query_id);

    void fetch_and_update_json_data(); 
    // Existing methods, if any, should be documented similarly.
    void log_performance_metric(const std::string& metric_name, double metric_value);
    std::vector<MetricStatistic> get_performance_statistics();
    nlohmann::json get_performance_statistics_json();
private:
    boost::asio::io_context& io_context_;  ///< Reference to the I/O context used for async operations.
    ssl::context& ssl_ctx_;
    Ollama ollama_;  ///< Instance of Ollama API handler.
    boost::asio::steady_timer timer_;  ///< Timer used for scheduling tasks or timeouts.
    std::shared_ptr<Client> client_; ///< Client used for making http requests
    std::queue<std::shared_ptr<Query>> query_queue_;  ///< Queue holding queries to be processed.
    std::unordered_map<std::string, std::shared_ptr<Query>> query_map_;  ///< Map from query IDs to their associated Query objects.
    std::mutex queue_mutex_;  ///< Mutex to protect access to the query queue and map.
    std::condition_variable queue_cv_;  ///< Condition variable to signal when new queries are added to the queue.
    std::unique_ptr<SQLite::Database> db_;
    /**
     * @brief Initializes the SQLite database connection.
     * 
     * Opens the SQLite database for the current date. If the database file does not exist, it is created.
     */
    void initialize_database();

    /**
     * @brief Checks if a table exists in the database and creates it if it doesn't.
     * 
     * Checks if the "example_table" exists in the SQLite database and creates it if it doesn't.
     */
    void check_and_create_tables();

    /**
     * @brief Continuously processes queries from the queue.
     * 
     * This function runs in a separate thread, popping queries from the queue and processing them.
     * If a query is canceled, it will be skipped.
     */
    void process_queries();

    /**
     * @brief Processes a single query by sending it to the LLM and handling partial responses.
     * 
     * This function sends the prompt to the LLM, handles partial responses, and marks the query as 
     * completed when all responses have been received or if an error occurs.
     * 
     * @param query The query to be processed.
     */
    void run_query(const std::shared_ptr<Query>& query);
};

#endif // APPLICATION_HPP


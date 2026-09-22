///////////////////////////////////////////////////////////////////////////////////
// File: WebSerial.h (FIXED)
//
// Description: Header file for custom WebSerial.
//              - Added printf declaration.
///////////////////////////////////////////////////////////////////////////////////

#ifndef WEBSERIAL_H
#define WEBSERIAL_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <functional>

// Define the function signature for the message callback
using WebSerialCallback = std::function<void(AsyncWebSocketClient* client, uint8_t* data, size_t len)>;

class WebSerialClass : public Stream {
public:
    // Start the WebSerial service and attach it to the web server
    void begin(AsyncWebServer* server, const char* url = "/ws");

    // Set the function to be called when a message is received
    void onMessage(WebSerialCallback callback);

    // Send a message to all connected clients
    void print(const String& message);
    void print(const char* message);
    void println(const String& message);
    void println(const char* message);
    
    // [BARU] Tambahkan deklarasi printf ini agar tidak error
    void printf(const char *format, ...);

     // -- IMPLEMENTASI DARI STREAM --
    virtual size_t write(uint8_t);
    virtual size_t write(const uint8_t *buffer, size_t size);
    virtual int available() { return 0; } 
    virtual int read() { return -1; }     
    virtual int peek() { return -1; }     
    virtual void flush() {}               

private:
    // Internal handler for WebSocket events
    static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);

    // Static members to implement the singleton pattern
    static AsyncWebSocket* _ws;
    static WebSerialCallback _messageCallback;
};

// Create a single global instance of the WebSerial class
extern WebSerialClass WebSerial;

#endif // WEBSERIAL_H

#include "UrlAssetAccessor.h"

#include <curl/curl.h>
#include <cstring>

class UrlAssetResponse : public CesiumAsync::IAssetResponse
{
public:
    virtual uint16_t statusCode() const override {}
    virtual std::string contentType() const override {}

    virtual const CesiumAsync::HttpHeaders& headers() const override
    {
        return _headers;
    }
    
    virtual gsl::span<const std::byte> data() const override
    {
        return gsl::span(const_cast<const std::byte*>(&result[0]), result.size());
    }

    static size_t headerCallback(char* buffer, size_t size, size_t nitems, void *userData);
    static size_t dataCallback(char* buffer, size_t size, size_t nitems, void *userData);
    void setCallbacks(CURL* curl);
    uint16_t _statusCode = 0;
    std::string _contentType;
    CesiumAsync::HttpHeaders _headers;
    std::vector<std::byte> _result;
};

class UrlAssetRequest : public CesiumAsync::IAssetRequest
{
public:
    UrlAssetRequest(const std::string& method, const std::string& url,
                    const CesiumAsync::HttpHeaders& headers)
        : _method(method), _url(url), _headers(headers)
    {
        
    }

    virtual const std::string& method() const
    {
        return this->_method;
    }

    virtual const std::string& url() const
    {
        return this->_url;
    }

    virtual const CesiumAsync::HttpHeaders& headers() const override
    {
        return _headers;
    }

    virtual const CesiumAsync::IAssetResponse* response() const override
    {
        return this->_pResponse.get();
    }

    void setResponse(std::unique_ptr<UrlAssetResponse> response)
    {
        _response = std::move(response);
    }
private:
    std::string _method;
    std::string _url;
    CesiumAsync::HttpHeaders _headers;
    std::unique_ptr<UrlAssetResponse> _response;
};

size_t UrlAssetResponse::headerCallback(char* buffer, size_t size, size_t nitems, void *userData)
{
    // size is supposed to always be 1, but who knows
    const size_t cnt = size * nitems;
    UrlAssetResponse* response = static_cast<UrlAssetResponse*>userData;
    if (!response)
        return cnt;
    char* colon = std::memchr(buffer, ':', nitems);
    if (colon)
    {
        char* value = colon + 1;
        char* end = buffer + cnt;
        while (value < end && *value == ' ')
            ++value;
        response->_headers.insert(std::string(buffer, colon), std::string(value, end));
    }
    return cnt;
}

extern "C" static size_t headerCallback(char* buffer, size_t size, size_t nitems, void *userData)
{
    return UrlAssetResponse::headerCallback(buffer, size, nitems, userData);
}

size_t UrlAssetResponse::dataCallback(char* buffer, size_t size, size_t nitems, void *userData)
{
    const size_t cnt = size * nitems;
    UrlAssetResponse* response = static_cast<UrlAssetResponse*>userData;
    if (!response)
        return cnt;
    response->_result.insert(response->_result.end(), buffer, buffer + cnt);
    return cnt;
}

extern "C" static size_t dataCallback(char* buffer, size_t size, size_t nitems, void *userData)
{
    return UrlAssetResponse::dataCallback(buffer, size, nitems, userData);
}

void UrlAssetResponse::setCallbacks(CURL* curl)
{
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ::dataCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ::headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
}

UrlAssetAccessor::UrlAssetAccessor()
    : _userAgent("Mozilla/5.0 Cesium for VSG")
{
    // XXX Do we need to worry about the thread safety problems with this?
    curl_global_init(CURL_GLOBAL_ALL);
}

UrlAssetAccessor::~UrlAssetAccessor()
{
    curl_global_cleanup();
}

static curl_slist* setCommonOptions(CURL* curl, const std::string& url, const std::string& userAgent,
                                    const std::vector<CesiumAsync::IAssetAccessor::THeader>& headers)
{
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_slist* list = nullptr;
    for (const CesiumAsync::IAssetAccessor::THeader& header : headers)
    {
        std::string fullHeader = header.first + ":" + header.second;
        list = curl_slist_append(list, fullHeader.c_str());
    }
    if (list)
    {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    return list;
}
    
CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>>
UnrealAssetAccessor::get(
    const CesiumAsync::AsyncSystem& asyncSystem,
    const std::string& url,
    const std::vector<CesiumAsync::IAssetAccessor::THeader>& headers)
{
    const auto& userAgent = _userAgent;
    return asyncSystem.createFuture<std::shared_ptr<CesiumAsync::IAssetRequest>>(
      [&url, &headers, &userAgent](const auto& promise)
      {
          std::shared_ptr<UrlAssetRequest> request
              = std::make_shared<UrlAssetRequest>("GET", url, headers);
          auto curl = curl_easy_init();
          curl_slist* list = setCommonOptions(curl, url, userAgent, headers);
          asyncSystem.runInWorkerThread([curl, list, promise, request]()
          {
              std::unique_ptr<UrlAssetResponse> response = std::make_unique<UrlAssetResponse>();
              response->setCallbacks(curl);
              CURLcode responseCode = curl_easy_perform(curl);
              curl_slist_free_all(list);
              if (responseCode == 0)
              {
                  long httpResponseCode = 0;
                  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpResponseCode);
                  response->_statusCode = reinterpret_cast<uint16_t>(httpResponseCode);
                  char *ct = nullptr;
                  curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
                  if (ct)
                  {
                      response->_contentType = ct;
                  }
                  request->_response = std::move(response);
                  promise.resolve(request);
              }
              else
              {
                  promise.reject(std::runtime_error(curl_easy_strerror(responseCode)));
              }
              curl_easy_cleanup(curl);
          });
      });
}

// request() with a verb and argument is essentially a POST

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>>
UrlAssetAccessor::request(const CesiumAsync::AsyncSystem& asyncSystem,
                          const std::string& verb,
                          const std::string& url,
                          const std::vector<CesiumAsync::IAssetAccessor::THeader>& headers,
                          const gsl::span<const std::byte>& contentPayload)
{
    const auto& userAgent = _userAgent;
    return asyncSystem.createFuture<std::shared_ptr<CesiumAsync::IAssetRequest>>(
        [&url, &headers, &userAgent, &verb, &contentPayload](const auto& promise)
        {
            std::shared_ptr<UrlAssetRequest> request
                = std::make_shared<UrlAssetRequest>(verb, url, headers);
            auto curl = curl_easy_init();
            curl_slist* list = setCommonOptions(curl, url, userAgent, headers);
            if (contentPayload.size() > 1UL << 31)
            {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, contentPayload.size());
            }
            else
            {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, contentPayload.size());
            }
            curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, reinterpret_cast<char*>(contentPayload.data()));
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, verb.c_str());
            asyncSystem.runInWorkerThread([curl, list, promise, request]()
            {
                std::unique_ptr<UrlAssetResponse> response = std::make_unique<UrlAssetResponse>();
                response->setCallbacks(curl);
                CURLcode responseCode = curl_easy_perform(curl);
                curl_slist_free_all(list);
                if (responseCode == 0)
                {
                    long httpResponseCode = 0;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpResponseCode);
                    response->_statusCode = reinterpret_cast<uint16_t>(httpResponseCode);
                    char *ct = nullptr;
                    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
                    if (ct)
                    {
                        response->_contentType = ct;
                    }
                    request->_response = std::move(response);
                    promise.resolve(request);
                }
                else
                {
                    promise.reject(std::runtime_error(curl_easy_strerror(responseCode)));
                }
                curl_easy_cleanup(curl);
            });
        });
}
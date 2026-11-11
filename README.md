# lightweight-http-server

Hiçbir harici kütüphane kullanmadan, doğrudan işletim sisteminin soket API'si üzerine yazılmış küçük bir HTTP/1.1 sunucusu. Modern C++17 ile yazıldı; Windows'ta Winsock, Linux ve macOS'ta BSD soketleriyle çalışıyor.

Bu projeyi yazmamın asıl sebebi şuydu: her gün kullandığımız `fetch`, `curl` ya da bir REST çağrısının arka planında aslında ne olduğunu kendi gözümle görmek. Express, Spring veya ASP.NET gibi çatılar bu katmanı o kadar iyi gizliyor ki, bir isteğin TCP bağlantısındaki ham baytlardan nasıl bir "request" nesnesine dönüştüğü çoğu zaman merak konusu olarak kalıyor. Burada o yolun tamamı, okunabilir birkaç yüz satır kodun içinde.

## Neler var?

- Ham soket üzerinde dinleme, `accept` döngüsü ve sabit boyutlu bir worker thread havuzu
- HTTP/1.0 ve HTTP/1.1 istek ayrıştırıcı (request line, header'lar, `Content-Length` ile gövde)
- Keep-alive ve pipelining desteği, boşta kalan bağlantılar için zaman aşımı
- `GET`, `POST`, `PUT`, `DELETE` için basit bir router; `HEAD` ve `OPTIONS` otomatik
- JSON ve düz metin yanıt üreticileri, hatalar için tutarlı bir JSON gövdesi
- Header ve gövde boyutu sınırları, dolu kuyrukta `503` ile geri basınç (back-pressure)
- Ctrl+C ile düzgün kapanma (graceful shutdown)
- Örnek API: `/api/status` ve `/api/telemetry`

## Derleme

Gereksinimler: C++17 destekleyen bir derleyici (GCC 9+, Clang 10+, MSVC 2019+) ve CMake 3.16 veya üstü.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Windows'ta MinGW kullanıyorsanız Ninja jeneratörünü tercih etmenizi öneririm:

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

> Küçük bir not: Proje klasörünün yolunda Türkçe karakter (ş, ç, ğ gibi) varsa `MinGW Makefiles` jeneratörü kaynak dosyaları bulamayabiliyor. Ninja'da ya da MSVC'de bu sorun yok.

## Çalıştırma

```bash
./build/lhs_server --port 8080 --threads 4
```

| Seçenek | Açıklama | Varsayılan |
|---|---|---|
| `-H, --host` | Dinlenecek IPv4 adresi | `0.0.0.0` |
| `-p, --port` | TCP portu | `8080` |
| `-t, --threads` | Worker thread sayısı | CPU çekirdek sayısı |
| `--max-body` | En büyük istek gövdesi (bayt) | `1048576` |
| `--keep-alive` | Boştaki bağlantının kapatılma süresi (ms) | `5000` |
| `-q, --quiet` | Erişim logunu kapatır | açık |
| `-v, --version` | Sürümü yazdırır | |
| `-h, --help` | Yardım metni | |

Sunucu her isteği şöyle bir satırla loglar:

```
[2026-11-11T07:49:29Z] 127.0.0.1:53038 "GET /api/status HTTP/1.1" 200 159B 0.04ms
```

## API uç noktaları

### `GET /api/status`

Servisin ayakta olup olmadığını ve temel çalışma bilgilerini döner. Bir load balancer'ın health check'i için birebir.

```bash
curl http://localhost:8080/api/status
```

```json
{
  "status": "ok",
  "service": "lightweight-http-server",
  "version": "1.0.0",
  "uptimeSeconds": 19.27,
  "workerThreads": 4,
  "activeConnections": 1,
  "timestamp": 1794383369
}
```

### `POST /api/telemetry`

Bir metrik örneği kaydeder. Gövde JSON ya da form formatında olabilir; gövde göndermeden query string ile de çalışır.

```bash
# JSON
curl -X POST -H "Content-Type: application/json" \
     -d '{"metric": "cpu_temp", "value": 61.5}' \
     http://localhost:8080/api/telemetry

# Form
curl -X POST -d "metric=cpu_temp&value=70.25" http://localhost:8080/api/telemetry

# Query string
curl -X POST "http://localhost:8080/api/telemetry?metric=fan.rpm&value=1200"
```

Başarılı kayıtta `201 Created` döner. Metrik adı `[A-Za-z0-9_.-]` karakterlerinden oluşmalı ve en fazla 64 karakter olmalı; değer sonlu bir sayı olmalı. Aksi halde `400` veya `422` alırsınız. Bellek şişmesin diye en fazla 64 farklı metrik tutuluyor, sınır dolunca yeni isimler `409` ile reddediliyor.

### `GET /api/telemetry`

Sunucunun kendi sayaçlarını ve kaydedilen metriklerin özetini (adet, min, max, ortalama, son değer) döner.

```json
{
  "server": {
    "uptimeSeconds": 19.57,
    "totalConnections": 11,
    "activeConnections": 1,
    "rejectedConnections": 0,
    "totalRequests": 7,
    "bytesReceived": 1160,
    "bytesSent": 2060,
    "responses": { "2xx": 5, "3xx": 0, "4xx": 2, "5xx": 0 }
  },
  "metrics": {
    "cpu_temp": { "count": 2, "min": 61.5, "max": 70.25, "avg": 65.875, "last": 70.25 },
    "fan.rpm":  { "count": 1, "min": 1200, "max": 1200, "avg": 1200, "last": 1200 }
  },
  "timestamp": 1794383369
}
```

### Diğerleri

- `GET /` uç noktaların listesini düz metin olarak verir.
- `POST /api/echo` gelen isteği (method, path, query, header'lar, gövde) JSON olarak geri yansıtır. Parser'ın neyi nasıl gördüğünü test etmek için kullanışlı.

Hatalar her zaman aynı biçimde döner:

```json
{ "error": "Not Found", "message": "No route for /nope", "status": 404 }
```

---

## HTTP alt seviyede nasıl işliyor?

Bu kısım projenin asıl derdi. Tarayıcıda bir adrese girdiğinizde ya da bir API çağırdığınızda aşağıda anlatılan adımların hepsi yaşanıyor; bu sunucu da her birini açıkça kodluyor.

### 1. Her şey bir TCP bağlantısıyla başlar

HTTP kendi başına bir taşıma protokolü değildir, TCP'nin üstünde çalışan metin tabanlı bir protokoldür. Sunucu tarafında bunun anlamı şu sistem çağrıları zinciridir:

```
socket()  ->  bind()  ->  listen()  ->  accept()  ->  recv()/send()  ->  close()
```

- `socket()` işletim sisteminden bir uç nokta ister.
- `bind()` bu uç noktayı bir IP adresi ve porta bağlar (örneğin `0.0.0.0:8080`).
- `listen()` soketi pasif moda alır. Çekirdek artık bu porta gelen bağlantıların TCP el sıkışmasını (SYN, SYN-ACK, ACK) kendisi tamamlar ve hazır bağlantıları bir kuyrukta biriktirir.
- `accept()` o kuyruktan bir bağlantı çeker ve bu tek istemciye özel yeni bir soket döner.

Önemli bir nokta: `accept()` döndüğünde el sıkışma çoktan bitmiştir. Uygulama sadece hazır bağlantıları teslim alır.

### 2. TCP bir akıştır, mesaj değil

HTTP'yi elle yazarken insanın ilk takıldığı yer burasıdır. TCP size "mesajlar" vermez, kesintisiz bir bayt akışı verir. İstemci tek seferde 300 baytlık bir istek gönderse bile `recv()` bunu 100 + 200 olarak iki parçada getirebilir. Tersi de olabilir: tek bir `recv()` çağrısı iki ayrı isteği birden içerebilir (bu HTTP pipelining'dir).

Bu yüzden sunucu gelen baytları bir tamponda biriktirir ve her yeni parçadan sonra şunu sorar: "Elimde tam bir istek var mı?" Cevap üç türlü olabilir ve kodda da tam olarak bu şekilde modellenmiştir:

```cpp
enum class ParseStatus { Complete, Incomplete, Error };
```

- `Incomplete`: Henüz yeterli veri yok, okumaya devam.
- `Complete`: Tam bir istek var; kaç bayt tükettiği de döner, kalan baytlar bir sonraki istek için tamponda kalır.
- `Error`: Protokole aykırı bir şey var; uygun durum koduyla yanıt verilip bağlantı kapatılır.

### 3. Bir HTTP isteğinin anatomisi

`curl -d '{"a":1}' -H 'Content-Type: application/json' http://localhost:8080/api/echo` komutu kabloya tam olarak şu baytları yazar (`\r\n` satır sonlarını temsil ediyor):

```
POST /api/echo HTTP/1.1\r\n
Host: localhost:8080\r\n
User-Agent: curl/8.0\r\n
Content-Type: application/json\r\n
Content-Length: 7\r\n
\r\n
{"a":1}
```

Üç bölüm var:

1. **Request line:** `METHOD SP target SP version`. Aralarında tam olarak birer boşluk olmalı. Parser, method'un RFC 9110'daki "token" karakterlerinden oluştuğunu, hedefin `/` ile başladığını ve sürümün `HTTP/1.0` ya da `HTTP/1.1` olduğunu doğrular. `HTTP/2.0` gibi bir sürüm gelirse `505 HTTP Version Not Supported` döner.
2. **Header'lar:** Her satır `İsim: değer` biçiminde. Header isimleri büyük/küçük harfe duyarsızdır, bu yüzden hepsi küçük harfe çevrilerek saklanır; `request.header("Content-Type")` ile `request.header("content-type")` aynı sonucu verir. Aynı isimde birden fazla header gelirse değerler virgülle birleştirilir.
3. **Boş satır ve gövde:** Header bloğu ilk `\r\n\r\n` dizisinde biter. Bu, parser'ın aradığı ilk işarettir; o görülmeden hiçbir şey ayrıştırılmaz.

Path ve query string de bu aşamada çözülür: `/api/echo?a=1&b=%20x` ifadesi `path = "/api/echo"` ve `query = { a: "1", b: " x" }` haline gelir. `%20` gibi yüzde kodlamaları (percent-encoding) da burada açılır.

### 4. Gövde nerede biter? Mesaj çerçeveleme

Header'ların nerede bittiğini boş satırdan anlıyoruz, peki gövde nerede biter? TCP bağlantısı açık kaldığı sürece "akışın sonu" diye bir işaret yok. HTTP/1.1 bu sorunu iki yolla çözer:

- `Content-Length: 7` → gövde tam olarak 7 bayttır.
- `Transfer-Encoding: chunked` → gövde, her biri kendi uzunluğunu taşıyan parçalar halinde gelir.

Bu sunucu yalnızca `Content-Length` destekliyor. `Transfer-Encoding` içeren istekleri `501 Not Implemented` ile açıkça reddediyor. Yarım yamalak bir destek vermektense net bir hata dönmek daha güvenli, çünkü bu iki başlığın birlikte yanlış yorumlanması "HTTP request smuggling" denen ciddi bir saldırı sınıfının kaynağı. Aynı sebeple birden fazla `Content-Length` header'ı gelirse istek `400` ile reddediliyor.

### 5. Yanıtın anatomisi

Yanıt da aynı yapıyı izler: durum satırı, header'lar, boş satır, gövde.

```
HTTP/1.1 200 OK\r\n
Date: Wed, 11 Nov 2026 07:49:29 GMT\r\n
Server: lightweight-http-server/1.0\r\n
Connection: keep-alive\r\n
Content-Length: 159\r\n
Content-Type: application/json; charset=utf-8\r\n
\r\n
{"status":"ok",...}
```

`HttpResponse::serialize()` bu baytları üretirken bazı kuralları kendisi uygular:

- `Content-Length` her zaman gövdenin gerçek boyutundan hesaplanır, handler'ın yanlış bir değer yazmasına izin verilmez.
- `HEAD` isteklerinde header'lar `GET` ile birebir aynıdır (`Content-Length` dahil) ama gövde gönderilmez.
- `204 No Content`, `304 Not Modified` ve `1xx` yanıtları standart gereği gövde taşıyamaz; bunlarda gövde ve `Content-Length` hiç yazılmaz.
- `Date` başlığı RFC 9110'daki IMF-fixdate biçiminde, her zaman GMT olarak üretilir.

### 6. Keep-alive ve pipelining

HTTP/1.0'da her istek için yeni bir TCP bağlantısı açılıp kapatılırdı. Her bağlantının bir el sıkışma maliyeti (ve HTTPS'te bunun üzerine TLS maliyeti) olduğu için HTTP/1.1 kalıcı bağlantıyı varsayılan yaptı. Sunucunun buradaki kararı şöyle:

- HTTP/1.1'de bağlantı açık kalır, ta ki istemci `Connection: close` gönderene kadar.
- HTTP/1.0'da bağlantı kapanır, istemci açıkça `Connection: keep-alive` istemediyse.
- Bir bağlantı en fazla 100 istek taşıyabilir; sonra sunucu `Connection: close` ile kapatır.
- Bağlantı belirli bir süre (varsayılan 5 sn) boşta kalırsa kapatılır. İstemci bir isteği yazmaya başlayıp yarıda bıraktıysa kapatmadan önce `408 Request Timeout` gönderilir.

Pipelining'de istemci yanıtları beklemeden arka arkaya birkaç istek yollar. Sunucu her döngüde soketten okumadan önce tamponu ayrıştırdığı için tamponda bekleyen ikinci isteği hemen işler ve yanıtları geliş sırasıyla gönderir.

### 7. `send()` her şeyi göndermeyebilir

`recv()` için geçerli olan parçalanma `send()` için de geçerli. 1 MB'lık bir yanıtı tek bir `send()` ile verdiğinizde çekirdek bunun yalnızca bir kısmını kabul edip size "şu kadarını yazdım" diyebilir. `sendAll()` fonksiyonu bu yüzden bir döngüdür: kalan bayt sıfır olana kadar göndermeye devam eder. Linux'ta karşı taraf bağlantıyı kapatmışken yazmaya çalışmak süreci `SIGPIPE` sinyaliyle öldürebileceği için bu sinyal göz ardı edilir ve hata `send()`'in dönüş değerinden okunur.

---

## Sunucu mimarisi

```
                   ┌──────────────────────────────┐
  TCP bağlantıları │  Accept thread                │
  ───────────────▶ │  poll(listener, 250ms)        │
                   │  accept() → kuyruğa ekle      │
                   └──────────────┬───────────────┘
                                  │  (kuyruk doluysa 503)
                                  ▼
                   ┌──────────────────────────────┐
                   │  Bekleyen bağlantı kuyruğu    │
                   │  std::queue + mutex + cv      │
                   └──────────────┬───────────────┘
                 ┌────────────────┼────────────────┐
                 ▼                ▼                ▼
          ┌────────────┐   ┌────────────┐   ┌────────────┐
          │  Worker 1  │   │  Worker 2  │   │  Worker N  │
          └─────┬──────┘   └────────────┘   └────────────┘
                │ handleConnection()
                ▼
     recv → HttpRequestParser → Router (dispatch) → Handler
                                                      │
     send ◀── HttpResponse::serialize() ◀─────────────┘
```

### Katmanlar

**Platform katmanı** (`HttpServer.cpp` içindeki anonim namespace). Winsock ile POSIX soketleri arasındaki tüm farklar burada toplanıyor: soket tipi (`SOCKET` vs `int`), kapatma fonksiyonu (`closesocket` vs `close`), hata kodları (`WSAGetLastError` vs `errno`), `poll` vs `WSAPoll`, zaman aşımı seçeneklerinin tipi (`DWORD` vs `timeval`). Header dosyası soketi platformdan bağımsız bir `std::intptr_t` olarak görüyor, böylece `#ifdef`'ler başka hiçbir yere sızmıyor.

**Parser** (`HttpRequestParser`). Durumsuz (stateless) tasarlandı: tamponun tamamını alır, sonucu döner. Bu, test edilmesini çok kolaylaştırıyor ve pipelining'i doğal olarak çözüyor. Header bloğu 8 KB'ı aşarsa `431`, gövde sınırı aşılırsa daha gövde okunmadan `413` döner; yani kötü niyetli bir istemci sunucuya gigabaytlarca veri yükleyemez.

**Router** (`HttpServer::dispatch`). Rotalar `path → (method → handler)` şeklinde iki seviyeli bir map'te tutuluyor. Bu yapı sayesinde 404 ile 405 ayrımı kendiliğinden ortaya çıkıyor: path yoksa `404`, path var ama method yoksa `405` ve yanıtta izin verilen methodları listeleyen bir `Allow` header'ı. `HEAD` istekleri `GET` handler'ına yönlendiriliyor, `OPTIONS` ise otomatik olarak `204` ve `Allow` listesiyle cevaplanıyor. Handler bir exception fırlatırsa worker thread çökmüyor; istemci `500` alıyor ve hata loglanıyor.

**Yanıt üretici** (`HttpResponse`, `JsonObject`). `HttpResponse::json()` ve `HttpResponse::text()` doğru `Content-Type` ile hazır yanıt üretir. `JsonObject` küçük, sadece yazmaya yönelik bir builder; string kaçışlarını (`\"`, `\n`, kontrol karakterleri) doğru yapıyor ve JSON'da karşılığı olmayan `NaN`/`Infinity` değerlerini `null` olarak yazıyor.

### Eşzamanlılık modeli

Sunucu "thread başına bağlantı" yerine sabit boyutlu bir worker havuzu kullanıyor. Accept thread'i yalnızca bağlantıyı kabul edip kuyruğa koyuyor; okuma, ayrıştırma ve yanıt üretme işini worker'lar yapıyor. Böylece aynı anda çok sayıda istemci geldiğinde sistem sınırsız thread açmıyor.

Kuyruğun da bir üst sınırı var (256). Bütün worker'lar meşgulken kuyruk da dolarsa yeni bağlantılar bekletilmek yerine anında `503 Service Unavailable` ile reddediliyor. Yük altında yavaşlayıp herkese kötü hizmet vermektense bir kısmına açıkça "sonra tekrar dene" demek genelde daha sağlıklı bir davranış.

Bunun bir bedeli de var: Bir worker, keep-alive bağlantısı boyunca o istemciye bağlı kalıyor. Çok sayıda boşta bekleyen bağlantı, worker'ları meşgul edebilir. Gerçek dünyadaki yüksek performanslı sunucular (nginx gibi) bu yüzden `epoll`/`kqueue`/IOCP tabanlı olay döngüleri kullanır. Bu proje okunabilirliği ön planda tuttuğu için bloklayan ama zaman aşımlı bir model seçti; keep-alive süresinin kısa tutulması da bu etkiyi sınırlıyor.

### Düzgün kapanma

Ctrl+C'ye basıldığında sinyal işleyici sadece `server.stop()` çağırıyor, o da yalnızca bir `std::atomic<bool>` bayrağını `false` yapıyor. Sinyal işleyici içinde mutex kilitlemek ya da condition variable uyandırmak güvenli değil, o yüzden bilerek bu kadar basit tutuldu. Accept döngüsü ve worker'lar zaten 250 ms'lik zaman aşımlarıyla bekledikleri için bayrağın değiştiğini kısa sürede fark ediyorlar. Ardından dinleyen soket kapatılıyor, worker'lar `join` ediliyor ve kuyrukta kalan bağlantılar temizleniyor.

### Güvenlik ve sınırlar özeti

| Durum | Davranış |
|---|---|
| Header bloğu 8 KB'ı aşıyor | `431 Request Header Fields Too Large` |
| Gövde `--max-body` sınırını aşıyor | `413 Content Too Large` (gövde okunmadan) |
| Hatalı request line ya da header | `400 Bad Request` |
| HTTP/1.1 isteğinde `Host` yok | `400 Bad Request` |
| Birden fazla `Content-Length` | `400 Bad Request` |
| `Transfer-Encoding` var | `501 Not Implemented` |
| Desteklenmeyen HTTP sürümü | `505 HTTP Version Not Supported` |
| İstek yarım bırakıldı | `408 Request Timeout` |
| Worker'lar ve kuyruk dolu | `503 Service Unavailable` |

## Kendi uç noktanızı eklemek

`src/main.cpp` içindeki `registerRoutes` fonksiyonuna bakmanız yeterli. Yeni bir rota eklemek birkaç satır:

```cpp
server.get("/api/hello", [](const lhs::HttpRequest& request) {
    const std::string name = request.queryParam("name").value_or("world");

    lhs::JsonObject body;
    body.add("message", "Hello, " + name + "!");
    return lhs::HttpResponse::json(body);
});
```

```bash
curl "http://localhost:8080/api/hello?name=Eren"
# {"message":"Hello, Eren!"}
```

## Proje yapısı

```
lightweight-http-server/
├── CMakeLists.txt          # C++17, lhs_core kütüphanesi + lhs_server CLI
├── include/
│   └── HttpServer.hpp      # Public API: request/response, parser, server
└── src/
    ├── HttpServer.cpp      # Soket katmanı, parser, router, yanıt üretici
    └── main.cpp            # CLI ve örnek REST uç noktaları
```

## Bilinçli olarak dışarıda bırakılanlar

Bu bir öğrenme ve deneme projesi; üretimde bir reverse proxy'nin arkasında bile doğrudan internete açmanızı önermem. Kapsam dışında kalanlar:

- TLS/HTTPS (önüne nginx veya Caddy koymak en pratik çözüm)
- `chunked` transfer encoding ve `Expect: 100-continue`
- IPv6
- Path parametreli rotalar (`/users/:id` gibi)
- HTTP/2 ve HTTP/3

Bunların bir kısmını ilerleyen sürümlerde eklemeyi düşünüyorum.

## Lisans

MIT. Ayrıntılar için [LICENSE](LICENSE) dosyasına bakabilirsiniz.

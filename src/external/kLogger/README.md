# kLogger (kemogu's Logger)

![Licence](https://img.shields.io/badge/license-MIT-blue.svg)
![C++ Version](https://img.shields.io/badge/C%2B%2B-17-green.svg)
![Structure](https://img.shields.io/badge/structure-header--only-brightgreen.svg)

**[English](#klogger-english) | [Türkçe](#klogger-türkçe)**

---

## kLogger (English)

**kLogger (kemogu's Logger)** is a fast, modern, and flexible **header-only** logging library for C++. This project aims to provide developers with a high-performance, easy-to-read logging solution that can be integrated into any C++ application with minimal effort.

### ✨ Features

* **Header-Only:** No compilation needed. Just include the headers and you're ready to go.
* **Lightweight & Fast:** Designed with performance-critical applications in mind.
* **Multi-level:** Standard logging levels (`INFO`, `WARNING`, `ERROR`).
* **Flexible Formatting:** Easily customize the format of your log messages.
* **Thread-Safe:** Safe to use in multi-threaded applications.
* **Multiple Sinks:** Direct logs to the console, files, or [other targets].

---

### 🚀 Getting Started

#### Requirements
* **C++17** compiler or newer.
* CMake 3.15+ (for integration).

#### Installation & Integration

**Option 1: Submodule / Add Subdirectory (Recommended)**

1.  Add kLogger to your project (e.g., inside `external/` folder):
    ```bash
    git submodule add [https://github.com/kemogu/kLogger.git](https://github.com/kemogu/kLogger.git) external/kLogger
    ```

2.  Update your `CMakeLists.txt`:
    ```cmake
    add_subdirectory(external/kLogger)
    
    add_executable(MyApp main.cpp)
    target_link_libraries(MyApp PRIVATE kLogger)
    ```

**Option 2: System-Wide Install**

1.  Build and install:
    ```bash
    git clone [https://github.com/kemogu/kLogger.git](https://github.com/kemogu/kLogger.git)
    cd kLogger && mkdir build && cd build
    cmake ..
    sudo cmake --install .
    ```

2.  Use in `CMakeLists.txt`:
    ```cmake
    find_package(kLogger REQUIRED)
    add_executable(MyApp main.cpp)
    target_link_libraries(MyApp PRIVATE kLogger::kLogger)
    ```

### 💻 Usage

Include the main header (or `kLogger.h`) and initialize the logger once at the start of your application.

```cpp
#include <KL/kLogger.h> // Includes Logger.h automatically

int main() {
    // 1. Initialize Logger (Optional: default path is current dir)
    // Args: Folder Path, Max Lines Per File
    KL::Logger::get_instance().init("logs", 5000);

    // 2. Console ONLY Logging (Fastest)
    LOG_INFO("Application started (Console only)");
    LOG_WARNING("This is a warning");
    
    // 3. File AND Console Logging
    FLOG_INFO("This goes to both console and the log file.");
    FLOG_ERROR("Critical error occurred! Saved to file.");

    // Note: The logger shuts down automatically when the program ends.
    return 0;
}
```
---

## kLogger (Türkçe)

**kLogger (kemogu's Logger)**, C++ için yazılmış hızlı, modern ve esnek bir **header-only** (yalnızca başlık dosyalarından oluşan) günlükleme (logging) kütüphanesidir. Bu proje, geliştiricilere yüksek performanslı, okunması kolay ve herhangi bir C++ uygulamasına minimum çabayla entegre edilebilen bir günlükleme çözümü sunmayı amaçlar.

### ✨ Özellikler

* **Header-Only:** Ekstra derleme adımı gerektirmez. Başlık dosyalarını projeye eklemeniz yeterlidir.
* **Hafif ve Hızlı:** Performansın kritik olduğu uygulamalar düşünülerek tasarlanmıştır.
* **Çok Seviyeli:** Standart günlükleme seviyeleri (`INFO`, `WARNING`, `ERROR`).
* **Esnek Formatlama:** Log mesajlarının formatını kolayca özelleştirebilirsiniz.
* **Thread-Safe (İş Parçacığı Güvenli):** Çok iş parçacıklı (multi-threaded) uygulamalarda güvenle kullanılabilir.
* **Çoklu Hedef (Sink):** Log’ları konsola, dosyalara veya [diğer hedeflere] yönlendirebilirsiniz.

---

### 🚀 Başlarken

#### Gereksinimler

* **C++17** (veya daha yeni) uyumlu bir derleyici (GCC, Clang, MSVC vb.)
* **CMake 3.15+** (projeye entegre etmek için)

---

### 🧩 Kurulum & Entegrasyon

kLogger, **header-only** bir kütüphane olmasına rağmen CMake ile rahat entegrasyon için bir yapı sunar. Aşağıdaki yöntemlerden birini kullanabilirsiniz.

#### Seçenek 1: Submodule / add_subdirectory (Önerilen)

1.  kLogger’ı projenize ekleyin (örneğin `external/` klasörü altına):
    ```bash
    git submodule add https://github.com/kemogu/kLogger.git external/kLogger
    ```

2.  `CMakeLists.txt` dosyanızı güncelleyin:
    ```cmake
    add_subdirectory(external/kLogger)

    add_executable(MyApp main.cpp)
    target_link_libraries(MyApp PRIVATE kLogger)
    ```

Bu yöntemle kLogger, projenizin bir parçası gibi derlenir ve CMake hedefi (`kLogger`) üzerinden bağlanır.

---

#### Seçenek 2: Sistem Genelinde Kurulum

1.  Depoyu klonlayın ve kurun:
    ```bash
    git clone https://github.com/kemogu/kLogger.git
    cd kLogger && mkdir build && cd build
    cmake ..
    sudo cmake --install .
    ```

2.  Projenizde `find_package` kullanarak kLogger’ı bulun:
    ```cmake
    find_package(kLogger REQUIRED)

    add_executable(MyApp main.cpp)
    target_link_libraries(MyApp PRIVATE kLogger::kLogger)
    ```

Bu yöntemle kLogger, sisteminizde global olarak kurulur ve bir CMake paketi olarak kullanılabilir.

---

### 💻 Kullanım

Uygulamanızın başlangıcında ana başlığı (veya doğrudan `kLogger.h` dosyasını) dahil edip logger’ı bir kez başlatmanız yeterlidir.

```cpp
#include <KL/kLogger.h> // Logger.h dosyasını da otomatik olarak dahil eder

int main() {
    // 1. Logger'ı başlatın (Opsiyonel: varsayılan klasör, çalıştığınız dizindir)
    // Argümanlar: Klasör Yolu, Dosya Başına Maksimum Satır Sayısı
    KL::Logger::get_instance().init("logs", 5000);

    // 2. Sadece Konsola Log (En hızlı yöntem)
    LOG_INFO("Uygulama başlatıldı (Sadece konsol)");
    LOG_WARNING("Bu bir uyarı mesajıdır");

    // 3. Dosya + Konsol Log
    FLOG_INFO("Bu mesaj hem konsola hem de log dosyasına yazılır.");
    FLOG_ERROR("Kritik bir hata oluştu! Dosyaya kaydedildi.");

    // Not: Program sona erdiğinde logger otomatik olarak kapanır.
    return 0;
}
```
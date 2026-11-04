#include "edgeinspect/inference_engine.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace edgeinspect {
namespace {
using Clock = std::chrono::steady_clock;
std::vector<std::uint8_t> read(const std::filesystem::path& p, std::uintmax_t limit) {
    if (!std::filesystem::is_regular_file(p)) throw std::runtime_error("Not a regular input file: " + p.string());
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open input: " + p.string());
    const auto n = f.tellg();
    if (n <= 0 || static_cast<std::uintmax_t>(n) > limit) throw std::runtime_error("Input is empty or exceeds byte limit");
    std::vector<std::uint8_t> b(static_cast<std::size_t>(n));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(b.data()), b.size())) throw std::runtime_error("Input read failed");
    return b;
}
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0])<<24) | (std::uint32_t(p[1])<<16) | (std::uint32_t(p[2])<<8) | p[3];
}
// Probe before pixel allocation. Limit supported color modes instead of silently
// changing CMYK/palette/16-bit conversion semantics relative to PIL.convert(RGB).
std::pair<int,int> dimensions(const std::vector<std::uint8_t>& b, std::uint64_t limit) {
    std::uint32_t w=0,h=0;
    const std::uint8_t png[]={137,80,78,71,13,10,26,10};
    if (b.size()>=33 && std::equal(png,png+8,b.begin())) {
        if (be32(b.data()+8)!=13 || std::string(b.begin()+12,b.begin()+16)!="IHDR" ||
            b[24]!=8 || (b[25]!=0 && b[25]!=2 && b[25]!=4 && b[25]!=6))
            throw std::runtime_error("Unsupported PNG mode: require 8-bit gray/RGB/RGBA");
        w=be32(b.data()+16); h=be32(b.data()+20);
    } else if (b.size()>=4 && b[0]==255 && b[1]==216) {
        bool eoi=false;
        for(std::size_t i=2;i+1<b.size();++i) if(b[i]==255 && b[i+1]==217) eoi=true;
        if(!eoi) throw std::runtime_error("Truncated JPEG: missing EOI");
        for(std::size_t i=2;i+3<b.size();) {
            if(b[i++]!=255) throw std::runtime_error("Malformed JPEG marker");
            while(i<b.size() && b[i]==255) ++i;
            if(i>=b.size()) break;
            const auto marker=b[i++];
            if(marker==218 || marker==217) break;
            if(marker==1 || (marker>=208 && marker<=215)) continue;
            if(i+2>b.size()) break;
            const auto n=(std::size_t(b[i])<<8)|b[i+1];
            if(n<2 || n>b.size()-i) throw std::runtime_error("Malformed JPEG segment");
            if((marker>=192 && marker<=195) || (marker>=197 && marker<=199) ||
               (marker>=201 && marker<=203) || (marker>=205 && marker<=207)) {
                if(n<8 || b[i+2]!=8 || (b[i+7]!=1 && b[i+7]!=3)) throw std::runtime_error("Unsupported JPEG mode");
                h=(std::uint32_t(b[i+3])<<8)|b[i+4]; w=(std::uint32_t(b[i+5])<<8)|b[i+6]; break;
            }
            i+=n;
        }
    } else throw std::runtime_error("Unsupported image: require JPEG or 8-bit PNG");
    if(!w || !h || w>32768 || h>32768 || std::uint64_t(w)*h>limit)
        throw std::runtime_error("Image dimensions invalid or exceed pixel/side limit");
    return {static_cast<int>(w),static_cast<int>(h)};
}
struct Taps { int first; std::vector<int> weights; };
// RGB-only adaptation of Pillow 11.3.0 Resample.c bilinear coefficients and
// two-pass 22-bit rounding. Copyright/license and source URL: third_party/Pillow-LICENSE.
std::vector<Taps> coefficients(int input) {
    const double scale=static_cast<double>(input)/320, support=std::max(1.0,scale);
    std::vector<Taps> all;
    for(int out=0;out<320;++out) {
        const double center=(out+0.5)*scale;
        const int first=std::max(0,static_cast<int>(center-support+0.5));
        const int end=std::min(input,static_cast<int>(center+support+0.5));
        std::vector<double> values; double sum=0;
        for(int i=first;i<end;++i) { const double v=std::max(0.0,1.0-std::abs((i-center+0.5)/support)); values.push_back(v); sum+=v; }
        Taps t{first,{}};
        for(double v:values) t.weights.push_back(static_cast<int>(0.5+v/sum*(1<<22)));
        all.push_back(std::move(t));
    }
    return all;
}
std::vector<std::uint8_t> resize_rgb(const cv::Mat& rgb) {
    const auto xs=coefficients(rgb.cols), ys=coefficients(rgb.rows);
    std::vector<std::uint8_t> horizontal(static_cast<std::size_t>(rgb.rows)*320*3), output(320*320*3);
    for(int y=0;y<rgb.rows;++y) for(int x=0;x<320;++x) for(int c=0;c<3;++c) {
        int sum=1<<21; const auto& t=xs[x];
        for(std::size_t i=0;i<t.weights.size();++i) sum+=rgb.ptr<std::uint8_t>(y)[(t.first+i)*3+c]*t.weights[i];
        horizontal[(y*320+x)*3+c]=static_cast<std::uint8_t>(std::clamp(sum>>22,0,255));
    }
    for(int y=0;y<320;++y) for(int x=0;x<320;++x) for(int c=0;c<3;++c) {
        int sum=1<<21; const auto& t=ys[y];
        for(std::size_t i=0;i<t.weights.size();++i) sum+=horizontal[((t.first+i)*320+x)*3+c]*t.weights[i];
        output[(y*320+x)*3+c]=static_cast<std::uint8_t>(std::clamp(sum>>22,0,255));
    }
    return output;
}
}
std::string sha256_bytes(const void* data, std::size_t n) {
    unsigned char hash[EVP_MAX_MD_SIZE]; unsigned length=0;
    if(EVP_Digest(data,n,hash,&length,EVP_sha256(),nullptr)!=1) throw std::runtime_error("SHA256 failed");
    std::ostringstream s; s<<std::hex<<std::setfill('0');
    for(unsigned i=0;i<length;++i) s<<std::setw(2)<<unsigned(hash[i]);
    return s.str();
}
std::string sha256_file(const std::filesystem::path& p) {
    const auto b=read(p,512*1024*1024); return sha256_bytes(b.data(),b.size());
}
PreparedInput prepare_image(const std::filesystem::path& path, const Config& config) {
    const auto start=Clock::now(); auto b=read(path,config.max_image_bytes);
    const auto [w,h]=dimensions(b,config.max_image_pixels);
    // Ignore EXIF orientation, as the original PIL pipeline does. No letterbox.
    auto image=cv::imdecode(b,cv::IMREAD_COLOR | cv::IMREAD_IGNORE_ORIENTATION);
    if(image.empty() || image.cols!=w || image.rows!=h) throw std::runtime_error("Image decode failed or dimensions changed");
    cv::Mat rgb; cv::cvtColor(image,rgb,cv::COLOR_BGR2RGB);
    PreparedInput out; out.original_width=w; out.original_height=h; out.rgb=resize_rgb(rgb);
    out.method="C++ OpenCV decode(ignore EXIF) + Pillow11.3-compatible RGB bilinear stretch320";
    out.input_sha256=sha256_bytes(b.data(),b.size()); out.rgb_sha256=sha256_bytes(out.rgb.data(),out.rgb.size());
    out.preprocessing_ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    return out;
}
PreparedInput prepare_rgb(const std::filesystem::path& path) {
    const auto start=Clock::now(); PreparedInput out; out.rgb=read(path,307200);
    if(out.rgb.size()!=307200) throw std::runtime_error("RGB input must contain exactly 307200 bytes");
    out.original_width=out.original_height=320; out.method="Exact RGB bytes; no JPEG decode/resize";
    out.input_sha256=out.rgb_sha256=sha256_bytes(out.rgb.data(),out.rgb.size());
    out.preprocessing_ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count(); return out;
}
} // namespace edgeinspect

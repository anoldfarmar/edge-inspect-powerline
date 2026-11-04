#include "edgeinspect/inference_engine.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace edgeinspect {
std::string json_string(const std::string& text) {
    std::ostringstream s; s<<'"';
    for(unsigned char c:text) {
        if(c=='"' || c=='\\') s<<'\\'<<c;
        else if(c<32) s<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;
        else s<<c;
    }
    s<<'"'; return s.str();
}
std::string result_json(const Result& r,const PreparedInput& p,const std::string& env) {
    std::ostringstream s; s<<std::setprecision(17);
    s<<"{\"asset\":"<<json_string(r.asset)<<",\"score\":"<<r.score<<",\"anomaly_threshold\":null,\"is_anomaly\":null"
     <<",\"decision\":\"UNCALIBRATED\",\"box\":[";
    for(int i=0;i<4;++i) s<<(i?",":"")<<r.selection.box[i];
    s<<"],\"confidence\":";
    if(r.selection.fallback) s<<"null"; else s<<r.selection.confidence;
    s<<",\"fallback\":"<<(r.selection.fallback?"true":"false")<<",\"bank_sha256\":"<<json_string(r.bank_sha256)
     <<",\"input_sha256\":"<<json_string(p.input_sha256)<<",\"rgb_sha256\":"<<json_string(p.rgb_sha256)
     <<",\"original_wh\":["<<p.original_width<<','<<p.original_height<<"],\"preprocessing\":"<<json_string(p.method)
     <<",\"timing_ms\":{\"preprocessing\":"<<p.preprocessing_ms<<",\"model\":"<<r.model_ms
     <<",\"detection_roi_normalization\":"<<r.detection_roi_ms<<",\"retrieval_top8\":"<<r.retrieval_ms
     <<",\"inference_total\":"<<r.inference_total_ms<<"},\"timing_scope\":\"steady_clock; model=inputs_set through outputs_get; preprocessing includes read/decode/resize/hash; inference_total excludes initialization and file saving, includes optional tensor copy; not camera FPS\""
     <<",\"environment\":"<<env<<'}'; return s.str();
}
void save_new_file(const std::filesystem::path& path,const void* data,std::size_t bytes) {
    const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(fd<0) throw std::runtime_error("Cannot create result "+path.string()+": "+std::strerror(errno));
    std::size_t done=0; const auto* p=static_cast<const char*>(data);
    while(done<bytes) {
        const auto n=::write(fd,p+done,bytes-done);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) {const int error=errno; ::close(fd); ::unlink(path.c_str()); throw std::runtime_error("Result write failed: "+std::string(std::strerror(error)));}
        done+=static_cast<std::size_t>(n);
    }
    if(::close(fd)!=0) {::unlink(path.c_str()); throw std::runtime_error("Result close failed");}
}
} // namespace edgeinspect

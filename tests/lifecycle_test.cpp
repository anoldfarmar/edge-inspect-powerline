#include "edgeinspect/inference_engine.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static std::size_t fds() {
    std::size_t n=0; for(const auto& p:std::filesystem::directory_iterator("/proc/self/fd")) {(void)p;++n;}return n;
}
int main(int argc,char** argv) {
    if(argc!=3) return 1;
    try {
        edgeinspect::Config c; c.root=argv[1];
        const auto input=edgeinspect::prepare_rgb(argv[2]);
        // Warm up runtime-global allocations once; compare later resource lifetimes.
        {edgeinspect::InferenceEngine warmup(c); warmup.infer(input,"glass-insulator");}
        const auto before=fds();
        for(int i=0;i<3;++i) {
            edgeinspect::InferenceEngine engine(c);
            bool bad=false;
            try{engine.infer({},"glass-insulator");}catch(const std::invalid_argument&){bad=true;}
            if(!bad) throw std::runtime_error("Empty RGB accepted");
            bad=false;
            try{engine.infer(input,"unknown");}catch(const std::invalid_argument&){bad=true;}
            if(!bad) throw std::runtime_error("Unknown asset accepted");
            const auto a=engine.infer(input,"glass-insulator").score;
            const auto b=engine.infer(input,"glass-insulator").score;
            if(a!=b) throw std::runtime_error("Repeat score mismatch");
        }
        // Failure AFTER rknn_init: omit banks. Constructor unwinding must destroy ctx.
        const auto dir=std::filesystem::temp_directory_path()/("edgeinspect-partial-init-"+std::to_string(getpid()));
        std::filesystem::create_directories(dir/"models");
        std::filesystem::create_symlink(std::filesystem::absolute(c.root/"models/insplad_shared_p3_320_fp.rknn"),dir/"models/insplad_shared_p3_320_fp.rknn");
        edgeinspect::Config broken;broken.root=dir;
        try {
            for(int i=0;i<3;++i) {
                bool failed=false; try{edgeinspect::InferenceEngine e(broken);}catch(const std::exception&){failed=true;}
                if(!failed) throw std::runtime_error("Missing bank accepted");
            }
        } catch(...) {std::filesystem::remove_all(dir);throw;}
        std::filesystem::remove_all(dir);
        const auto after=fds();
        if(after!=before) throw std::runtime_error("Open descriptor count changed after destruction");
        std::cout<<"{\"passed\":true,\"fd_before\":"<<before<<",\"fd_after\":"<<after
                 <<",\"successful_engine_lifecycles\":3,\"post_init_failure_lifecycles\":3,\"bad_request_then_valid\":true}\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}

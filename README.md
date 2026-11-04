# LMS-AdaptiveRegulator
## Abstract
Memory bandwidth contention in multi-core systems severely impacts application performance and quality-of-service (QoS) guarantees. Regulating the shared memory bandwidth mitigates the memory performance uncertainty thereby making it a manageable resource and improving trustworthiness of multicore system. In this work we propose a memory bandwidth regulation mechanism LMS-AR, i.e., LMS Prediction
based Adaptive Regulator within a Linux kernel module to distribute the memory bandwidth as a resource among the CPU cores. We describe a design in which both monitoring and regulation is enforced from outside by a master core which is not a dedicated controller for regulation. This allows for plugging in computationally heavy prediction and regulation algorithm without interfering with the regulated core. An
adaptive filtering technique was employed for prediction of percore bandwidth requirement. We conducted several experiments with SPEC CPU 2017 benchmarks distributed across multiple cores. Our proposed approach demonstrated significant improvement over Memguard with respect to slowdown ratios caused due to memory contention.

## Usage instructions 
 ```
  $ git clone https://github.com/ss22ongithub/LMS-AdaptiveRegulator.git
  $ cd LMS-AdaptiveRegulator 
  $ make
  $ insmod areg.ko
  $ echo 1 >  /sys/kernel/debug/ar/enable_regulation

  The regulation starts at this point . Tests can be run after this.

  To remove the module use
  $ rmmod areg.ko

   
  ```

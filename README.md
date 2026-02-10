# capstone-esp32

This monorepo contains all of the individual esp32 projects for our capstone.

### esp-now communication

Communication with our edge boards will be done over esp-now using a controller-peripheral model, as (currently) exemplified by the projects capstone-controller and capstone-peripheral. As part of this model, the file espnow\_types.h within the main directory of each project should be the same. **Be careful to keep it the same if you need to change it!**

### design considerations

All code should be done with the ESP32 ide, rather than using arduino headers, so that we can eventually work on power-saving measures, as well as work with freeRTOS threads.

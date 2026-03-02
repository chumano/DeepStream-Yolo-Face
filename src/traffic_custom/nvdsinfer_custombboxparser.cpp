#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <vector>
#include "nvdsinfer_logger.h"
#include "nvdsinfer_custom_impl.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLIP(a, min, max) (MAX(MIN(a, max), min))
// export PARSER_LOG_ENABLE=1
inline bool get_log_enable() {
  const char* env = std::getenv("PARSER_LOG_ENABLE");
  if (!env) return false;
  return (std::string(env) == "1" || std::string(env) == "true" || std::string(env) == "TRUE");
}

extern "C" bool NvDsInferParseCustomEfficientNMSTLT(
    std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferObjectDetectionInfo> &objectList);

extern "C" bool NvDsInferParseCustomEfficientNMSTLT(
    std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferObjectDetectionInfo> &objectList) {
  if (outputLayersInfo.size() != 4) {
    std::cerr << "Mismatch in the number of output buffers."
              << "Expected 4 output buffers, detected in the network :"
              << outputLayersInfo.size() << std::endl;
    return false;
  }

  /* Host memory for "EfficientNMS"
     Output layers are looked up by name to be robust against ordering
     differences when nvdsinferserver/Triton returns them.
     Expected layer names: num_dets, det_boxes, det_scores, det_classes
  */

  const NvDsInferLayerInfo *keepCount_layer = nullptr;
  const NvDsInferLayerInfo *bboxes_layer    = nullptr;
  const NvDsInferLayerInfo *scores_layer    = nullptr;
  const NvDsInferLayerInfo *classes_layer   = nullptr;

  for (const auto &layer : outputLayersInfo) {
    if (std::string(layer.layerName) == "num_dets")
      keepCount_layer = &layer;
    else if (std::string(layer.layerName) == "det_boxes")
      bboxes_layer = &layer;
    else if (std::string(layer.layerName) == "det_scores")
      scores_layer = &layer;
    else if (std::string(layer.layerName) == "det_classes")
      classes_layer = &layer;
  }

  if (!keepCount_layer || !bboxes_layer || !scores_layer || !classes_layer) {
    std::cerr << "EfficientNMS: could not find all required output layers by name. "
              << "Falling back to index order [0..3]." << std::endl;
    keepCount_layer = &outputLayersInfo[0];
    bboxes_layer    = &outputLayersInfo[1];
    scores_layer    = &outputLayersInfo[2];
    classes_layer   = &outputLayersInfo[3];
  }

  int   *p_keep_count = (int   *)keepCount_layer->buffer;
  float *p_bboxes     = (float *)bboxes_layer->buffer;
  float *p_scores     = (float *)scores_layer->buffer;
  int   *p_classes    = (int   *)classes_layer->buffer;

  const float threshold = detectionParams.perClassThreshold[0];

  // keep_top_k: det_boxes has dims [1, topk, 4] (batch dim included when
  // max_batch_size=0 in Triton config), so the topk count is in d[1].
  // Guard against models that omit the batch dim (numDims == 2 → d[0]).
  const int keep_top_k = (bboxes_layer->inferDims.numDims >= 3)
                         ? bboxes_layer->inferDims.d[1]
                         : bboxes_layer->inferDims.d[0];
  bool log_enable = false;//get_log_enable();
  
  if (log_enable) {
    std::cout << "keep cout: " << p_keep_count[0] << std::endl;
  }

  for (int i = 0; i < p_keep_count[0] && objectList.size() <= static_cast<size_t>(keep_top_k); i++) {
    if (static_cast<unsigned int>(p_classes[i]) >= detectionParams.numClassesConfigured)
      continue;

    if (p_scores[i] < 0.0)
      std::cout << "label/conf/ x/y x/y -- " << p_classes[i] << " "
                << p_scores[i] << " " << p_bboxes[4 * i] << " "
                << p_bboxes[4 * i + 1] << " " << p_bboxes[4 * i + 2] << " "
                << p_bboxes[4 * i + 3] << " " << std::endl;

    if (p_scores[i] < threshold)
      continue;

    if (log_enable) {
      std::cout << "label/conf/ x/y x/y -- " << p_classes[i] << " "
                << p_scores[i] << " " << p_bboxes[4 * i] << " "
                << p_bboxes[4 * i + 1] << " " << p_bboxes[4 * i + 2] << " "
                << p_bboxes[4 * i + 3] << " " << std::endl;
    }

    if (p_bboxes[4 * i + 2] < p_bboxes[4 * i] ||
        p_bboxes[4 * i + 3] < p_bboxes[4 * i + 1])
      continue;

    NvDsInferObjectDetectionInfo object;
    object.classId = p_classes[i];
    object.detectionConfidence = p_scores[i];

    /* Clip object box co-ordinates to network resolution */
    object.left = CLIP(p_bboxes[4 * i], 0, networkInfo.width - 1);
    object.top = CLIP(p_bboxes[4 * i + 1], 0, networkInfo.height - 1);
    object.width =
        CLIP(p_bboxes[4 * i + 2], 0, networkInfo.width - 1) - object.left;
    object.height =
        CLIP(p_bboxes[4 * i + 3], 0, networkInfo.height - 1) - object.top;

    if (object.height < 0 || object.width < 0)
      continue;

    // if (object.detectionConfidence < 0.8)
    //   continue;

    objectList.push_back(object);
  }
  return true;
}

/**
 * Custom parser for License Plate Recognition (LPR) model
 * NvDsInferClassiferParseCustomEfficientNMSLPR
*/

// Character mapping for LPR (36 classes: 0-9, A-Z)
static const char* LPR_CHARACTERS[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
    "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
    "U", "V", "W", "X", "Y", "Z"
};

extern "C" bool NvDsInferClassiferParseCustomEfficientNMSLPR(
    std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo,
    float classifierThreshold,
    std::vector<NvDsInferAttribute> &attrList,
    std::string &descString);

extern "C" bool NvDsInferClassiferParseCustomEfficientNMSLPR(
    std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo,
    float classifierThreshold,
    std::vector<NvDsInferAttribute> &attrList,
    std::string &descString) {
    
    if (outputLayersInfo.size() != 4) {
        std::cerr << "Mismatch in the number of output buffers."
                  << "Expected 4 output buffers, detected in the network :"
                  << outputLayersInfo.size() << std::endl;
        return false;
    }

    /* Host memory for "EfficientNMS"
       EfficientNMS has 4 output bindings, the order is:
       num_dets, det_boxes, det_scores, det_classes
    */
    int *p_num_dets = (int *)outputLayersInfo[0].buffer;
    float *p_det_boxes = (float *)outputLayersInfo[1].buffer;
    float *p_det_scores = (float *)outputLayersInfo[2].buffer;
    int *p_det_classes = (int *)outputLayersInfo[3].buffer;

    bool log_enable = get_log_enable();
    
    int num_detections = p_num_dets[0];
    
    if (log_enable) {
        std::cout << "LPR num detections: " << num_detections << std::endl;
        std::cout << "LPR classifier threshold: " << classifierThreshold << std::endl;
    }

    // Structure to hold character detections
    struct CharDetection {
        int class_id;
        float score;
        float x_center;
        float y_center;
        float x1, y1, x2, y2;
    };
    
    std::vector<CharDetection> valid_chars;
    
    // Extract valid character detections
    for (int i = 0; i < num_detections; i++) {
        if (p_det_classes[i] >= 36) // 36 classes total
            continue;
            
        if (p_det_scores[i] < classifierThreshold)
            continue;
            
        CharDetection char_det;
        char_det.class_id = p_det_classes[i];
        char_det.score = p_det_scores[i];
        char_det.x1 = p_det_boxes[4 * i];
        char_det.y1 = p_det_boxes[4 * i + 1];
        char_det.x2 = p_det_boxes[4 * i + 2];
        char_det.y2 = p_det_boxes[4 * i + 3];
        char_det.x_center = (char_det.x1 + char_det.x2) / 2.0f;
        char_det.y_center = (char_det.y1 + char_det.y2) / 2.0f;
        
        valid_chars.push_back(char_det);
        
        if (log_enable) {
            std::cout << "LPR char: class=" << char_det.class_id 
                      << " score=" << char_det.score
                      << " center=(" << char_det.x_center << "," << char_det.y_center << ")"
                      << std::endl;
        }
    }
    
    if (valid_chars.empty()) {
        descString = "";
        return true;
    }
    
    // Determine if it's a 1-row or 2-row layout
    float min_y = valid_chars[0].y_center;
    float max_y = valid_chars[0].y_center;
    float avg_char_height = 0.0f;
    
    for (const auto& char_det : valid_chars) {
        min_y = MIN(min_y, char_det.y_center);
        max_y = MAX(max_y, char_det.y_center);
        avg_char_height += (char_det.y2 - char_det.y1);
    }
    avg_char_height /= valid_chars.size();
    
    float y_range = max_y - min_y;
    bool is_two_row = y_range > (avg_char_height * 0.3f);
    
    if (log_enable) {
        std::cout << "LPR layout: " << (is_two_row ? "2-row" : "1-row") 
                  << " y_range=" << y_range << " avg_height=" << avg_char_height << std::endl;
    }
    
    // Sort characters based on layout
    if (is_two_row) {
        // Sort by y first (top to bottom), then by x (left to right)
        float y_threshold = min_y + y_range / 2.0f;
        
        std::vector<CharDetection> top_row, bottom_row;
        
        // Separate into rows
        for (const auto& char_det : valid_chars) {
            if (char_det.y_center <= y_threshold) {
                top_row.push_back(char_det);
            } else {
                bottom_row.push_back(char_det);
            }
        }
        
        // Sort each row by x-coordinate
        std::sort(top_row.begin(), top_row.end(), 
                  [](const CharDetection& a, const CharDetection& b) {
                      return a.x_center < b.x_center;
                  });
        
        std::sort(bottom_row.begin(), bottom_row.end(), 
                  [](const CharDetection& a, const CharDetection& b) {
                      return a.x_center < b.x_center;
                  });
        
        // Combine top row first, then bottom row
        valid_chars.clear();
        valid_chars.insert(valid_chars.end(), top_row.begin(), top_row.end());
        valid_chars.insert(valid_chars.end(), bottom_row.begin(), bottom_row.end());
        
    } else {
        // Sort by x-coordinate only (left to right)
        std::sort(valid_chars.begin(), valid_chars.end(), 
                  [](const CharDetection& a, const CharDetection& b) {
                      return a.x_center < b.x_center;
                  });
    }
    
    // Build license plate text using character mapping
    std::string plate_text = "";
    for (const auto& char_det : valid_chars) {
        if (char_det.class_id < 36 && char_det.class_id >= 0) { // 36 classes total
            plate_text += LPR_CHARACTERS[char_det.class_id];
            
            if (log_enable) {
                std::cout << "Class ID: " << char_det.class_id 
                          << ", Label: '" << LPR_CHARACTERS[char_det.class_id] 
                          << "', Score: " << char_det.score << std::endl;
            }
        }
    }
    
    descString = plate_text;
    
    if (log_enable) {
        std::cout << "LPR recognized text: " << plate_text << std::endl;
    }
    
    // Create attribute for the recognized text
    if (!plate_text.empty()) {
        NvDsInferAttribute attr;
        attr.attributeIndex = 0;
        attr.attributeValue = 0;
        attr.attributeConfidence = 1.0f;
        attr.attributeLabel = strdup(plate_text.c_str());
        attrList.push_back(attr);
    }
    
    return true;
}

CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(NvDsInferParseCustomEfficientNMSTLT);
CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE(NvDsInferClassiferParseCustomEfficientNMSLPR);
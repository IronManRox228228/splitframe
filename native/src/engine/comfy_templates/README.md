# ComfyUI Workflow Templates

This directory contains built-in workflow templates for SplitFrame's ComfyUI integration.

## Creating and Importing Custom Templates (e.g. LTX Video)

1. **Build and test your workflow in ComfyUI:**
   - Create your workflow in the ComfyUI web UI (for example, an LTX-Video text-to-video or image-to-video setup).
   - Ensure you enable "Enable Dev mode Options" in the ComfyUI settings dialog.

2. **Export the workflow in API format:**
   - In the ComfyUI menu, click **Save (API Format)** or **Export (API)** to download the API JSON file.
   - Note the node IDs for the prompt, seed, dimensions, frame count, input image, and the final output node (such as `SaveAnimatedWEBP`, `SaveImage`, or `VHS_VideoCombine`).

3. **Import the template into SplitFrame:**
   Use the `comfy.template.import` engine command:
   ```json
   {
     "id": "ltx_video",
     "name": "LTX Video text to video",
     "kind": "video",
     "description": "Generate video using Lightricks LTX-Video model",
     "whenToUse": ["b-roll", "establishing", "action"],
     "path": "/path/to/ltx_api.json",
     "outputNode": "12",
     "bindings": {
       "prompt": {"node": "6", "input": "text"},
       "negative": {"node": "7", "input": "text"},
       "seed": {"node": "3", "input": "seed"},
       "width": {"node": "5", "input": "width"},
       "height": {"node": "5", "input": "height"},
       "frames": {"node": "5", "input": "length"}
     }
   }
   ```
   SplitFrame validates that all referenced node IDs and inputs exist, verifies the output node, and saves the template to the user's template directory (`SF_COMFY_TEMPLATES` or app data location).

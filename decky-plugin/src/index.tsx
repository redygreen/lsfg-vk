import { staticClasses } from "@decky/ui";
import { definePlugin } from "@decky/api";
import { GiPlasticDuck } from "react-icons/gi";
import { Content } from "./components/Content";

export default definePlugin(() => {
  console.log("decky-lsfg-vk-adaptive plugin initializing");

  return {
    name: "LSFG Adaptive",
    titleView: <div className={staticClasses.Title}>LSFG Adaptive</div>,
    alwaysRender: true,
    content: <Content />,
    icon: <GiPlasticDuck />,
    onDismount() {
      console.log("decky-lsfg-vk-adaptive unloading");
    }
  };
});

module.exports = async function ({github, context}, dataJson) {
  console.log(github)
  console.log(context)
  console.log("dataJson is\n", dataJson)
}

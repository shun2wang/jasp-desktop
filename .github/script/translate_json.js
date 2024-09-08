module.exports = async function ({github, context}, dataJson) {
  console.log(github)
  console.log(context)
    const fs = require('fs/promises');

  fs.readFile('./Resources/Data Sets/index.json')
    .then((data) => {
      console.log(data);
      console.log(JSON.parse(data));
    })
    .catch((error) => {
      console.log(error)
    });
}
